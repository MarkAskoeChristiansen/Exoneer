// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"

/**
 * Pure attitude-control library for the reaction-wheel triad. SI throughout
 * (N*m, N*m*s, kg*m^2, rad, rad/s). No UObject, no engine state, no frame
 * globals - everything here is a function of its arguments, which is what lets
 * Exoneer.Attitude.* test it directly.
 *
 * The device being modelled is a three-axis reaction-wheel triad: three rotors
 * on orthogonal axes inside one block. It is an INTERNAL actuator, so it obeys
 * three hard physical facts, all of which live in this file:
 *
 *   1. Rated torque per axis. Each rotor motor has a maximum torque; the
 *      command is clamped per axis (BoundToCube), not by vector magnitude,
 *      because a real triad delivers its rating on each axis independently.
 *
 *   2. Momentum capacity per axis. A rotor at its speed limit cannot absorb
 *      more momentum, so torque in the winding direction stops. The capacity
 *      is the rotor's I*omega, authored on the block definition - NOT
 *      "rating x some seconds", which is not a property any hardware has.
 *
 *   3. The gyroscopic reaction -w x h. A wound rotor resists rotation about
 *      the other axes. It is power-neutral by construction (it is always
 *      perpendicular to w), so it can never add or remove rotational energy;
 *      Exoneer.Attitude.CrossTermIsPowerNeutral asserts exactly that.
 *
 * CONTROL LAW. The vehicle-level loop is rate-command / attitude-hold, the
 * same scheme a fly-by-wire helicopter or a spacecraft ACS uses: the pilot
 * commands a body RATE, that rate integrates an attitude REFERENCE, and a PD
 * law drives the hull onto the reference.
 *
 * The reference holds ROLL AND PITCH ONLY; yaw is released every frame
 * (ReleaseBodyAxis) so heading stays on pure rate, and releasing the stick
 * slews the reference back to level (LevelReference) at an authored rate. A
 * thrust vehicle has no restoring moment about its own centre of mass, so an
 * arbitrary bank is a PERMANENT sideways acceleration and a 20 degree bump
 * left the craft accelerating at 3.6 m/s^2 for ever. Returning to level is
 * the only thing that ends it, and it is why the reference is here at all.
 *
 * WHAT PAYS FOR IT. Holding any rate against the hull's angular damping costs
 * D*I*w of rotor momentum EVERY SECOND, so the rotors alone can only ever
 * hold a turn for a while - see MaxCommandRateAboutAxisRadS, which budgets
 * exactly that, and HoldTorqueNm, which is the quantity differential thrust
 * takes over (ExoneerThrust.h). Rotors pay for transients; thrust pays for
 * anything that does not decay. That division is the architecture.
 *
 * The gains are not tuned by hand - they are derived from the hull's measured
 * inertia tensor and one authored settle time:
 *
 *     wn = 1 / SettleTimeSeconds            (rad/s, natural frequency)
 *     Kp = wn^2 * I                         (N*m per rad)
 *     Kd = (2 * zeta * wn - D_hull) * I     (N*m per rad/s)
 *
 * where D_hull is the angular damping the rigid body already applies (Chaos
 * angular damping is a torque -D*I*w, so it is subtractable in exactly these
 * units). Targeting zeta = 1 gives a critically damped closed loop: the
 * commanded attitude is reached with NO overshoot at any inertia, and the
 * craft holds it when the stick is released.
 *
 * Because Kd/I = 2*zeta*wn is a constant independent of the hull, the explicit
 * game-thread integration is stable at any frame rate a player will ever see:
 * the discrete loop gain is dt * 2*zeta*wn, which at the AUTHORED 0.25 s settle
 * time (DA_Block_Gyro attitude_settle_time_seconds) is 0.131 at 60 fps, 0.393
 * at 20 fps and 0.785 at 10 fps, against a stability limit of 2. The old flat
 * "gain x rating" law had a loop gain of 0.87 at 60 fps on the roll axis and
 * diverged below 26 fps; that is the ringing the pilot felt.
 *
 * The derived cascade leash follows from the same number: RateLimit *
 * SettleTime = 20 deg/s * 0.25 s = 5.0 degrees. (Both figures were quoted from
 * a 0.7 s settle time this file no longer authors; they are restated here from
 * the bootstrap so the next reader is not deriving gains from a stale comment.)
 *
 * THE REFERENCE IS BOUNDED IN ABSOLUTE TERMS TOO - see LimitReferenceTilt. The
 * leash bounds how far the reference may sit from the HULL, which stops windup
 * and says nothing at all about where the hull ends up: full-deflection roll
 * rolled the hull to 53.9 degrees in 3 s, 93.2 degrees in 5 s and 168.7
 * degrees in 10 s on one held key, and a thrust craft banked past its own
 * hold-weight angle is not flying, it is falling with the engine pointing
 * sideways. A bank ceiling is a flight-envelope limiter, not a fudge: it
 * removes an ATTITUDE COMMAND the craft cannot hold its own weight at, and a
 * collision can still put the hull anywhere it likes.
 */
namespace ExoneerAttitude
{
	/** Authored properties of one reaction-wheel triad block. */
	struct FGyroSpec
	{
		/** Rated motor torque PER AXIS (N*m). */
		float RatedTorqueNm = 0.f;

		/** Rotor momentum capacity PER AXIS (N*m*s). This is I_rotor * omega_max. */
		float MomentumCapacityNms = 0.f;
	};

	/** Everything the vehicle-level loop needs to produce a torque. */
	struct FLoopParams
	{
		/** Hull inertia about the centre of mass, body principal axes (kg*m^2). */
		FVector InertiaKgM2 = FVector(1.f, 1.f, 1.f);

		/** Closed-loop time constant 1/wn (s). Authored on the gyro block. */
		float SettleTimeSeconds = 0.7f;

		/** Target damping ratio. 1.0 = critically damped = no overshoot. */
		float DampingRatio = 1.f;

		/** Angular damping the rigid body already applies (1/s); subtracted from Kd. */
		float HullAngularDamping = 0.f;
	};

	/** Natural frequency of the attitude loop (rad/s). */
	inline float NaturalFrequency(const FLoopParams& P)
	{
		return 1.f / FMath::Max(P.SettleTimeSeconds, KINDA_SMALL_NUMBER);
	}

	/** Spring gain per axis, N*m per rad. Kp = wn^2 * I. */
	inline FVector PositionGain(const FLoopParams& P)
	{
		const float Wn = NaturalFrequency(P);
		return P.InertiaKgM2 * (Wn * Wn);
	}

	/**
	 * Damping gain per axis, N*m per rad/s, NET of the damping the rigid body
	 * already supplies. Kd = (2*zeta*wn - D_hull) * I, floored at zero: if the
	 * hull is already damped past the target the triad adds none.
	 */
	inline FVector RateGain(const FLoopParams& P)
	{
		const float Needed = 2.f * P.DampingRatio * NaturalFrequency(P);
		return P.InertiaKgM2 * FMath::Max(0.f, Needed - P.HullAngularDamping);
	}

	/**
	 * Rotation vector (axis * angle, rad) that takes Actual onto Reference,
	 * expressed in Actual's own body axes. Shortest arc; the w<0 flip keeps a
	 * 179 deg error from reading as -181.
	 */
	inline FVector AttitudeErrorBody(const FQuat& Actual, const FQuat& Reference)
	{
		FQuat Error = Actual.Inverse() * Reference;
		Error.Normalize();
		if (Error.W < 0.f)
		{
			Error = FQuat(-Error.X, -Error.Y, -Error.Z, -Error.W);
		}
		const FVector Axis = FVector(Error.X, Error.Y, Error.Z);
		const double SinHalf = Axis.Size();
		if (SinHalf <= UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			return FVector::ZeroVector;
		}
		const double Angle = 2.0 * FMath::Atan2(SinHalf, Error.W);
		return Axis * (Angle / SinHalf);
	}

	/**
	 * Largest attitude error the triad can hold statically, per axis (rad):
	 * the error at which the SPRING term alone asks for full rated torque.
	 * This is the bound for a loop that has a position gain. The vehicle loop
	 * is a cascade instead - the error becomes a bounded rate command, see
	 * MaxTrackableErrorRad - so this and PositionGain describe the direct-PD
	 * form the library also supports and the tests exercise.
	 */
	inline FVector MaxHoldableErrorRad(const FLoopParams& P, float TotalRatedTorqueNm)
	{
		const FVector Kp = PositionGain(P);
		return FVector(
			TotalRatedTorqueNm / FMath::Max(Kp.X, UE_DOUBLE_KINDA_SMALL_NUMBER),
			TotalRatedTorqueNm / FMath::Max(Kp.Y, UE_DOUBLE_KINDA_SMALL_NUMBER),
			TotalRatedTorqueNm / FMath::Max(Kp.Z, UE_DOUBLE_KINDA_SMALL_NUMBER));
	}

	/**
	 * Effective inertia about an arbitrary unit axis, body frame (kg*m^2).
	 * Exact for the diagonal (principal-axes) tensor the hull reports:
	 * I_eff = u . I u = sum u_i^2 I_i.
	 *
	 * This exists because the pilot's stick is indexed about the COCKPIT's
	 * axes, not the hull's. Indexing a per-axis rate limit by the body axis
	 * number and then applying it about a cockpit axis is only right when the
	 * cockpit is mounted square, which is true of the shipped rover and false
	 * of the first craft a player builds with a rotated seat - there the pitch
	 * ask was being scaled by the ROLL axis budget.
	 */
	inline float EffectiveInertiaAboutAxis(const FVector& InertiaKgM2, const FVector& UnitAxisBody)
	{
		const FVector U = UnitAxisBody.GetSafeNormal();
		return static_cast<float>(U.X * U.X * InertiaKgM2.X
			+ U.Y * U.Y * InertiaKgM2.Y
			+ U.Z * U.Z * InertiaKgM2.Z);
	}

	/**
	 * Leash for a CASCADE hold, per axis (rad): the attitude error at which
	 * the outer loop already asks for the full commanded rate, which is
	 * RateLimit * SettleTime. Nothing is gained by letting the reference sit
	 * further away than this and windup is what is lost, so this is the bound
	 * the flight loop uses. It is derived from two numbers the loop already
	 * has rather than authored a third time.
	 */
	inline FVector MaxTrackableErrorRad(const FLoopParams& P, const FVector& RateLimitRadS)
	{
		return RateLimitRadS * FMath::Max(P.SettleTimeSeconds, KINDA_SMALL_NUMBER);
	}

	/**
	 * Largest body rate the pilot may command about one axis (rad/s). THREE
	 * limits, all physical:
	 *
	 *   - the spin-up cost. Reaching rate w costs I*w of rotor momentum.
	 *
	 *   - THE HOLD COST, which the previous pass did not budget and which is
	 *     what took the yaw axis out. A rigid body with angular damping D
	 *     needs a torque D*I*w to STAY at rate w, for ever, and a reaction
	 *     wheel pays that in momentum at D*I*w N*m*s per second. Budgeting
	 *     only I*w said the yaw axis could hold 30 deg/s; the truth was that
	 *     holding it cost a further 120 N*m*s every second and the rotor hit
	 *     its stop after 202 degrees of one continuous turn - one mouse hold.
	 *     So the budget pays for the spin-up PLUS SustainedTurnSeconds of
	 *     holding it out of the rotors alone:
	 *
	 *         I*w + D*I*w*T <= Fraction * Capacity
	 *         w <= Fraction * Capacity / (I * (1 + D*T))
	 *
	 *     Past T the hold cost is carried by differential thrust instead (see
	 *     ExoneerThrust.h) on any axis where the craft's thrust geometry can
	 *     make that moment. T is therefore the guarantee for a craft that has
	 *     NO such geometry, not a limit on turn duration.
	 *
	 *   - an authored comfort ceiling, so a feather-light hull does not spin
	 *     faster than a pilot can read.
	 *
	 * Install a second gyro and Capacity doubles, so the craft genuinely turns
	 * faster - progression by hardware, not by a difficulty setting.
	 */
	inline float MaxCommandRateAboutAxisRadS(const FLoopParams& P, const FVector& UnitAxisBody,
		float TotalMomentumCapacityNms, float MomentumFraction, float CeilingRadS,
		float SustainedTurnSeconds)
	{
		const double Budget = FMath::Max(0.f, TotalMomentumCapacityNms) * FMath::Clamp(MomentumFraction, 0.f, 1.f);
		const double HoldFactor = 1.0 + FMath::Max(0.f, P.HullAngularDamping) * FMath::Max(0.f, SustainedTurnSeconds);
		const double Inertia = FMath::Max(EffectiveInertiaAboutAxis(P.InertiaKgM2, UnitAxisBody),
			UE_KINDA_SMALL_NUMBER);
		return static_cast<float>(FMath::Min(static_cast<double>(CeilingRadS), Budget / (Inertia * HoldFactor)));
	}

	/** The same limit on each body principal axis, for readouts and tests. */
	inline FVector MaxCommandRateRadS(const FLoopParams& P, float TotalMomentumCapacityNms,
		float MomentumFraction, float CeilingRadS, float SustainedTurnSeconds)
	{
		return FVector(
			MaxCommandRateAboutAxisRadS(P, FVector::XAxisVector, TotalMomentumCapacityNms,
				MomentumFraction, CeilingRadS, SustainedTurnSeconds),
			MaxCommandRateAboutAxisRadS(P, FVector::YAxisVector, TotalMomentumCapacityNms,
				MomentumFraction, CeilingRadS, SustainedTurnSeconds),
			MaxCommandRateAboutAxisRadS(P, FVector::ZAxisVector, TotalMomentumCapacityNms,
				MomentumFraction, CeilingRadS, SustainedTurnSeconds));
	}

	/**
	 * Torque needed to HOLD a body rate against the hull's own angular
	 * damping (N*m, per axis). This is the quantity a reaction wheel cannot
	 * afford indefinitely and differential thrust can, so it is also the size
	 * of the offload the trim path is asked for.
	 */
	inline FVector HoldTorqueNm(const FLoopParams& P, const FVector& BodyRateRadS)
	{
		return BodyRateRadS * P.InertiaKgM2 * FMath::Max(0.f, P.HullAngularDamping);
	}

	/**
	 * The PD attitude law, body axes. AttitudeErrorRad is the rotation that
	 * takes the hull onto the reference; RateErrorRadS is (commanded rate -
	 * actual rate). Both are body-frame.
	 */
	inline FVector SolveBodyTorque(const FLoopParams& P, const FVector& AttitudeErrorRad, const FVector& RateErrorRadS)
	{
		const FVector Kp = PositionGain(P);
		const FVector Kd = RateGain(P);
		return AttitudeErrorRad * Kp + RateErrorRadS * Kd;
	}

	/** Rebuild a reference from an attitude error expressed in Actual's body axes. */
	inline FQuat ReferenceFromBodyError(const FQuat& Actual, const FVector& ErrorRad)
	{
		const double Angle = ErrorRad.Size();
		const FQuat Delta = Angle > UE_DOUBLE_KINDA_SMALL_NUMBER
			? FQuat(ErrorRad / Angle, Angle) : FQuat::Identity;
		FQuat Out = Actual * Delta;
		Out.Normalize();
		return Out;
	}

	/**
	 * Pull the reference back so it never sits further from the hull than the
	 * triad can hold statically. This is the anti-windup: the classic failure
	 * is an integrator that walks away while the hull is held by something
	 * else and then releases its whole error at once, and expressing the limit
	 * as a REFERENCE bound rather than an output clamp is what stops it.
	 */
	inline FQuat LeashReference(const FQuat& Reference, const FQuat& Actual, const FVector& MaxErrorRad)
	{
		const FVector Error = AttitudeErrorBody(Actual, Reference);
		const FVector Limited(
			FMath::Clamp(Error.X, -MaxErrorRad.X, MaxErrorRad.X),
			FMath::Clamp(Error.Y, -MaxErrorRad.Y, MaxErrorRad.Y),
			FMath::Clamp(Error.Z, -MaxErrorRad.Z, MaxErrorRad.Z));
		if (Limited.Equals(Error, UE_DOUBLE_KINDA_SMALL_NUMBER))
		{
			return Reference;
		}
		return ReferenceFromBodyError(Actual, Limited);
	}

	/**
	 * Drop the component of the reference's error about one BODY axis, so that
	 * axis is left on pure rate command and nothing is held there.
	 *
	 * The flight loop holds ROLL and PITCH and leaves YAW on rate. A held
	 * heading is a compass the pilot did not ask for and cannot see; held roll
	 * and pitch are the difference between a craft that stays where you left
	 * it and one that keeps whatever bank a bump gave it - which, with no
	 * gravity restoring moment about the centre of mass, is for ever.
	 */
	inline FQuat ReleaseBodyAxis(const FQuat& Reference, const FQuat& Actual, int32 Axis)
	{
		FVector Error = AttitudeErrorBody(Actual, Reference);
		if (Axis >= 0 && Axis < 3)
		{
			Error[Axis] = 0.0;
		}
		return ReferenceFromBodyError(Actual, Error);
	}

	/**
	 * Slew the reference toward "same heading, no roll, no pitch" by at most
	 * MaxStepRad, so its HEADING does not move at all - only its roll and
	 * pitch. An inverted reference comes back the short way round.
	 *
	 * This is the pilot's way back to level, and it is the whole answer to
	 * "flying feels so weird". A thrust vehicle has no restoring moment about
	 * its centre of mass, so an arbitrary bank is a permanent sideways
	 * acceleration; the only thing that can end it is the attitude system
	 * being asked to. Releasing the stick asks.
	 */
	inline FQuat LevelReference(const FQuat& Reference, float MaxStepRad)
	{
		if (MaxStepRad <= 0.f)
		{
			return Reference;
		}
		// The target is the reference's own HEADING with no roll and no pitch.
		// Building it from the heading rather than rotating the up axis onto
		// world up is what makes this wings-level rather than an autopilot:
		// rotating about the axis perpendicular to the two up vectors walks the
		// azimuth of the forward axis, so the craft would slowly turn.
		const FVector Forward = Reference.GetAxisX();
		const FVector Flat(Forward.X, Forward.Y, 0.0);
		if (Flat.IsNearlyZero())
		{
			// Nose straight up or straight down: there is no heading to keep,
			// so leave it to the pilot rather than pick one.
			return Reference;
		}
		FQuat Target = FRotationMatrix::MakeFromXZ(Flat.GetSafeNormal(), FVector::UpVector).ToQuat();
		Target.Normalize();

		FQuat Delta = (Target * Reference.Inverse()).GetNormalized();
		if (Delta.W < 0.0)
		{
			Delta = FQuat(-Delta.X, -Delta.Y, -Delta.Z, -Delta.W);
		}
		const FVector DeltaVector(Delta.X, Delta.Y, Delta.Z);
		const double Sine = DeltaVector.Size();
		if (Sine <= UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			return Target;   // already level
		}
		const double Angle = 2.0 * FMath::Atan2(Sine, Delta.W);
		if (Angle <= static_cast<double>(MaxStepRad))
		{
			return Target;
		}
		FQuat Out = FQuat(DeltaVector / Sine, static_cast<double>(MaxStepRad)) * Reference;
		Out.Normalize();
		return Out;
	}

	/**
	 * Hold the reference's own lift axis within MaxTiltRad of world up, taking
	 * the shortest way back and keeping its heading. Returns it unchanged when
	 * it is already inside the cone, and when the limit is not a limit.
	 *
	 * THIS IS THE ABSOLUTE BOUND THE LEASH IS NOT. LeashReference bounds the
	 * reference relative to the HULL, which is anti-windup and says nothing
	 * about where the hull is going; A/D is full-deflection roll and the
	 * reference integrated straight through 90 degrees of bank in 5 seconds of
	 * one held key (measured: 53.9 degrees at 3 s, 93.2 at 5 s, 168.7 at 10 s).
	 * Past the angle at which the reserved ceiling can no longer hold weight the
	 * craft is not manoeuvring, it is falling with its lift vector pointing
	 * sideways, and past 90 degrees the same held key used to be a powered dive
	 * at 2.2 g (see ExoneerThrust::HoverCollective).
	 *
	 * The limiter is honest about what it is: it refuses to COMMAND an attitude
	 * the craft cannot hold its weight at. It applies no torque of its own, it
	 * has no authority the triad does not already have, and it does not stop a
	 * collision, a slope or a hard landing putting the hull anywhere - the
	 * reference then re-seeds from the hull and the pilot flies it back.
	 */
	inline FQuat LimitReferenceTilt(const FQuat& Reference, float MaxTiltRad)
	{
		if (MaxTiltRad <= 0.f || MaxTiltRad >= PI)
		{
			return Reference;
		}
		const FVector LiftAxis = Reference.GetAxisZ();
		const double Tilt = FMath::Acos(FMath::Clamp(LiftAxis.Z, -1.0, 1.0));
		if (Tilt <= static_cast<double>(MaxTiltRad))
		{
			return Reference;
		}
		// LevelReference walks toward wings-level along the shortest arc while
		// keeping the heading, so stepping by exactly the excess lands on the
		// cone rather than somewhere inside it.
		return LevelReference(Reference, static_cast<float>(Tilt - MaxTiltRad));
	}

	/**
	 * Tilt of a reference's lift axis away from world up (rad). The readout
	 * side of LimitReferenceTilt, and what a test asserts against.
	 */
	inline float ReferenceTiltRad(const FQuat& Reference)
	{
		return static_cast<float>(FMath::Acos(FMath::Clamp(Reference.GetAxisZ().Z, -1.0, 1.0)));
	}

	/**
	 * Advance the attitude reference by the commanded body rate, then leash
	 * it. Reference and Actual are world-frame; CommandedBodyRate is body.
	 */
	inline FQuat AdvanceReference(const FQuat& Reference, const FQuat& Actual,
		const FVector& CommandedBodyRateRadS, float DeltaSeconds, const FVector& MaxErrorRad)
	{
		FQuat Next = Reference;
		if (!CommandedBodyRateRadS.IsNearlyZero() && DeltaSeconds > 0.f)
		{
			const FVector Step = CommandedBodyRateRadS * DeltaSeconds;
			const double Angle = Step.Size();
			Next = Reference * FQuat(Step / Angle, Angle);
			Next.Normalize();
		}
		return LeashReference(Next, Actual, MaxErrorRad);
	}

	/**
	 * First-order lag: the SUSTAINED part of a torque, i.e. what is left after
	 * everything shorter than TimeConstantSeconds has been filtered out.
	 *
	 * The split matters because the two halves have different actuators. A
	 * transient belongs to the rotors, which give the momentum back when the
	 * stick is released. Anything that does NOT decay - a standing moment, or
	 * the torque needed to hold a rate against hull damping - has to be paid
	 * by thrust, because a rotor paying it fills on a stopwatch.
	 */
	inline FVector AdvanceSustainedTorque(const FVector& Current, const FVector& InstantNm,
		float DeltaSeconds, float TimeConstantSeconds)
	{
		const float Alpha = FMath::Clamp(DeltaSeconds / FMath::Max(TimeConstantSeconds, KINDA_SMALL_NUMBER), 0.f, 1.f);
		return Current + (InstantNm - Current) * Alpha;
	}

	/**
	 * Gyroscopic reaction on the hull from rotors holding momentum h while the
	 * hull turns at w: -w x h. Perpendicular to w by construction, so
	 * dot(result, w) == 0 and the term transports no power.
	 */
	inline FVector GyroscopicTorque(const FVector& BodyRateRadS, const FVector& StoredMomentumNms)
	{
		return -FVector::CrossProduct(BodyRateRadS, StoredMomentumNms);
	}

	/**
	 * Zero the components of a commanded torque that would wind an already
	 * saturated rotor further. Both arguments are in the triad's own axes.
	 * The rotor winds OPPOSITE the torque it delivers, so the winding rate on
	 * axis i is -Command[i].
	 */
	inline FVector ApplySaturation(const FVector& CommandTorqueLocal, const FVector& StoredMomentumLocal, float CapacityNms)
	{
		FVector Out = CommandTorqueLocal;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double Stored = StoredMomentumLocal[Axis];
			const double Winding = -Out[Axis];
			if (FMath::Abs(Stored) >= CapacityNms && Stored * Winding > 0.0)
			{
				Out[Axis] = 0.f;
			}
		}
		return Out;
	}

	/** Fraction of the per-axis momentum envelope in use, 0..1 (worst axis). */
	inline float SaturationFraction(const FVector& StoredMomentumLocal, float CapacityNms)
	{
		if (CapacityNms <= KINDA_SMALL_NUMBER)
		{
			return 0.f;
		}
		return FMath::Clamp(static_cast<float>(StoredMomentumLocal.GetAbsMax()) / CapacityNms, 0.f, 1.f);
	}
}
