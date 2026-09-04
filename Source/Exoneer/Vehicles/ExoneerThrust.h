// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

/**
 * Pure thrust-routing library: the lift valve and the differential-thrust trim
 * allocator. SI throughout (N, N*m, throttle units, seconds). No UObject, no
 * engine state - everything is a function of its arguments, which is what lets
 * Exoneer.Thrust.* test it directly.
 *
 * TWO RULES SHAPE THIS FILE, and both come from a playtest that went wrong.
 *
 * 1. THE LIFT KEY IS A VALVE, NOT AN INTEGRATOR. Move.Z is a lift LEVEL: 1
 *    while the key is held, 0 the moment it is released. AdvanceCollective
 *    FOLLOWS that level at the valve's authored slew rate, so releasing the
 *    key drives the setting to zero and thrust stops. A previous pass made
 *    the key integrate a collective whose zero meant HOLD; the craft then
 *    kept climbing after the pilot let go, which is not a control at all -
 *    there was no key that closed the valve and no readout of where the lever
 *    had been left. The slew rate is kept, because a real valve cannot step
 *    from 0 to 24 kN in one frame; the latch is not.
 *
 * 2. THRUST PAYS FOR WHAT DOES NOT DECAY; ROTORS PAY FOR TRANSIENTS. That is
 *    the whole division of labour, and the three jobs on the thrust side are
 *    all the same job seen from different angles. Every one of them is made
 *    ZERO-SUM IN FORCE by NullNetTrimForce before it is committed, so a trim
 *    is a pure moment and the pilot's altitude and ground track do not move
 *    while it works:
 *
 *      a. HOLDING A STANDING MOMENT. A thrust layout whose net moment about
 *         the centre of mass is non-zero at the pilot's own throttle setting
 *         makes a moment that never ends. A reaction wheel integrates that
 *         straight into its rotors - StandingMomentNm is exactly the quantity
 *         - so paying for it with rotor momentum fills the store at
 *         |M| N*m*s per second and the axis dies on a stopwatch. Thrust,
 *         however, can cancel thrust for free and for ever.
 *
 *      b. HOLDING A RATE. This is the one the previous pass missed. A hull
 *         with angular damping D needs D*I*w of torque to STAY at rate w, for
 *         ever; on the test rover that is 120 N*m in yaw at 30 deg/s, which
 *         is 120 N*m*s of rotor momentum every second and the axis gone after
 *         one continuous 202 degree turn. So the SUSTAINED part of the
 *         attitude command - a first-order lag, ExoneerAttitude::
 *         AdvanceSustainedTorque - is offloaded here too. A transient still
 *         never comes here: at the authored lag a quarter-second stick input
 *         passes almost nothing, and a multi-second hold passes all of it.
 *
 *      c. UNWINDING STORED MOMENTUM against an external torque, which is the
 *         only way a rotor ever sheds any.
 *
 *    An axis where the craft's thrust geometry can make no moment at all gets
 *    none of this, and then (b) is a countdown the visor has to show. The
 *    shipped rover's six lift nozzles are CANTED a few degrees, outboard on
 *    each rail, precisely so yaw is not that axis: a diagonal trim pattern on
 *    the four corner units is a pure yaw couple with zero net force, while
 *    uniform throttle makes no yaw at all. Nothing is free - the cant costs
 *    cos^2 of the installed lift, visibly, on the TWR readout.
 *
 * 3. THE PILOT'S THROTTLE STOPS SHORT OF THE STOP, AND THE BOUND IS
 *    ASYMMETRIC. A valve may be biased down as far as it can close and up as
 *    far as it can open - TrimBoundMin and TrimBoundMax, which are simply the
 *    valve's own travel. A single symmetric bound was pessimistic by half at
 *    every setting off the middle and said a shut valve could never open,
 *    which is not true of any valve. What remains true is that a valve at a
 *    STOP has no travel in that direction, so PilotThrottleCeiling reserves
 *    the last slice of the lever as control margin: full collective is where
 *    a climbing pilot lives and it is exactly where (a) to (c) are needed. It
 *    costs thrust-to-weight, honestly and visibly.
 */
namespace ExoneerThrust
{
	/**
	 * Follow a lift LEVEL at the valve's authored slew rate (throttle units
	 * per second). TargetLevel is the pilot's key state: 1 held, 0 released.
	 * Zero in means the valve closes - the property the regression removed.
	 */
	inline float AdvanceCollective(float Current, float TargetLevel, float DeltaSeconds, float SlewPerSec)
	{
		return FMath::FInterpConstantTo(
			FMath::Clamp(Current, 0.f, 1.f),
			FMath::Clamp(TargetLevel, 0.f, 1.f),
			FMath::Max(DeltaSeconds, 0.f),
			FMath::Max(SlewPerSec, KINDA_SMALL_NUMBER));
	}

	/**
	 * The largest base throttle the PILOT may command, leaving ReserveFraction
	 * of every valve's travel as control margin.
	 *
	 * Without this the differential-thrust path is unreachable at the top of
	 * the lever, which is exactly where a climbing pilot sits: a valve pinned
	 * at 1.00 has no travel left to open, and a force-neutral moment needs
	 * units moving BOTH ways. Stopping the pilot's travel at 1 - reserve keeps
	 * that authority alive all the way up.
	 *
	 * The BOTTOM needs no reserve, because a shut valve can still open - see
	 * TrimBoundMax. What a shut valve cannot do is help make a moment that
	 * adds no net force, since there is then nothing left to take the force
	 * back with, and the visor says so rather than pretending.
	 *
	 * The cost is real and is shown to the pilot: the craft's ascent
	 * thrust-to-weight is the reserved ceiling times the installed lift, not
	 * the installed lift.
	 */
	inline float PilotThrottleCeiling(float ReserveFraction)
	{
		return 1.f - FMath::Clamp(ReserveFraction, 0.f, 0.5f);
	}

	/**
	 * Valve setting that holds altitude: the collective at which world-vertical
	 * lift equals weight, plus a vertical-RATE term so the craft settles onto
	 * zero climb instead of drifting off the feed-forward's own error.
	 *
	 * This is a throttle governor, not an autopilot and not a force. It moves
	 * nothing but the valve, inside the valve's own travel and slew rate, and
	 * every newton it commands is a thruster burning power. A craft whose
	 * ceiling cannot reach the hover setting - too heavy, banked too far,
	 * browned out - simply runs the valve to the stop and sinks, which is what
	 * a real machine does.
	 *
	 * There is deliberately NO position term. An altitude integrator is a
	 * latch: it would fight the pilot, wind up while the craft sits on its
	 * suspension, and have no key that switches it off. With a rate term only,
	 * the governor's whole state is the vertical speed the pilot can see.
	 *
	 * VerticalLiftScaleN is the NET world-vertical lift the craft makes at
	 * collective 1 (N), so a banked craft is asked for the larger setting it
	 * really needs. It is SIGNED - the per-unit world-up share is a dot
	 * product, not max(0, dot) - because the whole question past 90 degrees of
	 * bank is whether opening the valve raises the craft or drives it into the
	 * ground, and a sum of clamped terms can only ever answer "raises".
	 * CraftLiftScaleN is the same figure along the craft's OWN lift axis, i.e.
	 * what it would make level, and the two together are what let the governor
	 * tell "banked too far" from "cannot lift its own weight" from "pointing at
	 * the ground". They are three different failures and want three different
	 * answers.
	 *
	 * TargetVerticalSpeedMS is the reference the rate term flies, so the
	 * descend key is a bounded descent RATE rather than a valve kill.
	 */
	inline float HoverCollective(float WeightN, float VerticalLiftScaleN, float CraftLiftScaleN,
		float VerticalSpeedMS, float TargetVerticalSpeedMS, float DampingPerMS, float Ceiling,
		float HeldSetting)
	{
		const float Cap = FMath::Max(Ceiling, 0.f);
		// PAST 90 DEGREES: the lift vector has no upward component left, so
		// every newton the valve makes is a newton DOWNWARD. Shut it.
		//
		// This branch is first because it is the only honest answer to the
		// state, whatever else is true of the craft. Without it the freeze
		// below fired instead - AlongWorldUp used to be clamped at zero, so
		// VerticalLiftScaleN * Cap < WeightN was permanently true from 90
		// degrees onward and the freeze held the valve at whatever the pilot
		// had, which at that boundary is ALWAYS the ceiling by construction.
		// The result was a POWERED DIVE. Measured on the shipped rover from
		// 200 m with one held roll key: 3 s reached 53.9 degrees of bank and
		// cost 0.6 m, 5 s reached 93.2 degrees and 14.5 m, and 10 s reached
		// 168.7 degrees with 21.3 kN of thrust pointing at the ground - 1.2 g
		// ON TOP of weight, so 2.2 g of downward acceleration - by which point
		// 288 m had gone and the craft was doing -104 m/s against a
		// landing-damage threshold of 8 m/s. A shut valve pointing down is 1 g,
		// which is strictly better and is what a machine with no lift left
		// really does.
		//
		// It is also CONTINUOUS in the quantity that matters. The signed scale
		// crosses zero exactly at the attitude where the craft makes no net
		// vertical force at any valve setting, so the 0.90 -> 0 step in the
		// VALVE is a 0 N -> 0 N step in vertical FORCE. The old clamped scale
		// put that step at 120 degrees of bank, where it was worth 10.7 kN.
		if (VerticalLiftScaleN <= KINDA_SMALL_NUMBER)
		{
			return 0.f;
		}
		// UNDERPOWERED: no attitude would help, so run the valve to the stop
		// and sink. That is what a real machine does and the honest answer.
		if (CraftLiftScaleN * Cap < WeightN)
		{
			return Cap;
		}
		// BANKED PAST HOLDING WEIGHT: the craft COULD hover level, so the
		// governor has nothing useful to say about this attitude and stops
		// answering - it FREEZES the valve where the pilot last had it.
		//
		// The band this covers is 32 to 90 degrees of bank on the shipped
		// rover: the craft could hold weight level, cannot hold it here, and
		// the lift still points upward, so the honest thing is to leave the
		// valve alone and let it sink. Frozen is continuous at the LOWER
		// boundary - the governed value at 32 degrees is exactly the ceiling -
		// and it cannot run away, because the ceiling still bounds it and every
		// shut-off path still closes it. The UPPER boundary is the branch above.
		if (VerticalLiftScaleN * Cap < WeightN)
		{
			return FMath::Clamp(HeldSetting, 0.f, Cap);
		}
		const float FeedForward = WeightN / VerticalLiftScaleN;
		const float Rate = -(VerticalSpeedMS - TargetVerticalSpeedMS) * FMath::Max(DampingPerMS, 0.f);
		return FMath::Clamp(FeedForward + Rate, 0.f, Cap);
	}

	/**
	 * True when the craft's ATTITUDE has taken the governor's authority away:
	 * it could hold weight level and cannot hold it here, so the valve is
	 * frozen and the craft is sinking. The visor says PINNED rather than
	 * HOVER, because they are different machine states and only one of them is
	 * holding altitude. An underpowered craft is not pinned - it is simply at
	 * the stop, which the LIFT percentage already says.
	 */
	inline bool IsHoverGovernorPinned(float WeightN, float VerticalLiftScaleN, float CraftLiftScaleN,
		float Ceiling)
	{
		const float Cap = FMath::Max(Ceiling, 0.f);
		return VerticalLiftScaleN > KINDA_SMALL_NUMBER
			&& CraftLiftScaleN * Cap >= WeightN
			&& VerticalLiftScaleN * Cap < WeightN;
	}

	/**
	 * True when the craft's lift has no upward component left at all, so the
	 * valve is shut and nothing the pilot does with it will help. A DIFFERENT
	 * machine state from pinned, and the one the visor has to name: pinned is
	 * sinking with the valve where the pilot left it, this is falling with the
	 * engine off because the engine is pointing at the ground.
	 */
	inline bool IsLiftInverted(float VerticalLiftScaleN)
	{
		return VerticalLiftScaleN <= KINDA_SMALL_NUMBER;
	}

	/**
	 * True only when stored rotor momentum has climbed past the authored
	 * desaturation onset. This is the ONLY gate that lets thruster trim move:
	 * an ordinary attitude command returns false here, so the valves stay
	 * where the pilot put them.
	 */
	inline bool ShouldDesaturate(float Saturation01, float OnsetFraction, float ReleaseFraction,
		float DumpRatePerSec, bool bAlreadyDesaturating)
	{
		if (DumpRatePerSec <= 0.f)
		{
			return false;
		}
		// HYSTERESIS, because without it an unwind stops the instant
		// saturation drops back below the onset and the axis PARKS there: 30 s
		// of continuous pitch left the store sitting at 80 percent of the
		// envelope for the rest of the flight, which is 45 percent of the
		// commanded rate in one direction and not the fresh envelope a green
		// readout implies. Once started, the dump runs down to the release
		// fraction.
		const float Threshold = bAlreadyDesaturating
			? FMath::Min(ReleaseFraction, OnsetFraction)
			: OnsetFraction;
		return Saturation01 > Threshold;
	}

	/** One thruster as the trim allocator sees it. */
	struct FTrimEffector
	{
		/** Moment about the centre of mass per unit of throttle (N*m), world frame. */
		FVector MomentPerUnitThrottle = FVector::ZeroVector;

		/** Force along the craft's LIFT axis per unit of throttle (N). Sideways units contribute 0. */
		float LiftPerUnitThrottle = 0.f;

		/**
		 * FULL force per unit of throttle (N), same frame as the moment. The
		 * lift row alone was not enough: a forward-facing unit has
		 * LiftPerUnitThrottle = 0 and was therefore free to the allocator
		 * while changing the craft's ground track by up to 400 N, and a canted
		 * lift nozzle leaks horizontally in proportion to its cant. Nulling
		 * the force VECTOR is what makes "a trim is a pure moment" true rather
		 * than true of one component.
		 */
		FVector ForcePerUnitThrottle = FVector::ZeroVector;

		/** Throttle the pilot asked for, before any trim. */
		float BaseThrottle = 0.f;

		/** How far this unit may be biased either way, before the 0..1 throttle range is applied. */
		float TrimFraction = 0.f;

		/** Bias last frame; the slew limit is measured from here. */
		float PreviousTrim = 0.f;

		/** Bias this frame. In/out. */
		float Trim = 0.f;
	};

	/** Moment the current trims actually deliver (N*m). */
	inline FVector DeliveredTrimTorqueNm(TArrayView<const FTrimEffector> Effectors)
	{
		FVector Total = FVector::ZeroVector;
		for (const FTrimEffector& Effector : Effectors)
		{
			Total += Effector.MomentPerUnitThrottle * Effector.Trim;
		}
		return Total;
	}

	/**
	 * Moment the pilot's own throttle setting makes about the centre of mass
	 * (N*m), before any trim. This is the STANDING moment - it does not decay,
	 * it is not a transient, and it is the single number that decides whether
	 * a thrust layout is flyable:
	 *
	 *   - fed to the trim allocator with a minus sign, thrust cancels it and
	 *     it costs no rotor momentum at all;
	 *   - left to the triad, it fills the momentum store at |M| N*m*s per
	 *     second, so a 1224 N*m moment against a 1600 N*m*s store takes the
	 *     pitch axis out in 1.3 seconds and nothing can then stop the craft
	 *     rotating. That is what the shipped test rover used to do the moment
	 *     the pilot held the forward key.
	 *
	 * Whatever the allocator cannot cancel is the residual the pilot must be
	 * told about, because on a badly balanced build it is a countdown.
	 */
	inline FVector StandingMomentNm(TArrayView<const FTrimEffector> Effectors)
	{
		FVector Total = FVector::ZeroVector;
		for (const FTrimEffector& Effector : Effectors)
		{
			Total += Effector.MomentPerUnitThrottle * Effector.BaseThrottle;
		}
		return Total;
	}

	/** Net force the current trims add along the craft's lift axis (N). Must be zero at commit. */
	inline float NetTrimLiftN(TArrayView<const FTrimEffector> Effectors)
	{
		double Total = 0.0;
		for (const FTrimEffector& Effector : Effectors)
		{
			Total += static_cast<double>(Effector.LiftPerUnitThrottle) * Effector.Trim;
		}
		return static_cast<float>(Total);
	}

	/**
	 * How far UP one unit may be biased: its authored fraction, or whatever
	 * travel it has left to the stop, whichever is smaller.
	 *
	 * The bound used to be a single SYMMETRIC number - the smaller of the two
	 * directions - on the argument that a force-neutral moment needs units
	 * moving both ways. That is true of the SET and not of one valve, and
	 * taking it per valve was pessimistic by half at every setting off the
	 * middle: at the hover collective a unit could really close by 0.35 and
	 * was told 0.245, and a shut valve was told it could not open at all.
	 *
	 * What is still true is that a valve at a stop has no travel in that
	 * direction. PilotThrottleCeiling is what keeps the top reachable; the
	 * bottom needs no reserve, but a craft with EVERY lift valve shut has
	 * nothing left to take the force back with, and the visor says so.
	 */
	inline float TrimBoundMax(const FTrimEffector& Effector)
	{
		return FMath::Max(0.f, FMath::Min(FMath::Clamp(Effector.TrimFraction, 0.f, 1.f),
			1.f - FMath::Clamp(Effector.BaseThrottle, 0.f, 1.f)));
	}

	/** How far DOWN one unit may be biased: it can close as far as it is open. */
	inline float TrimBoundMin(const FTrimEffector& Effector)
	{
		return -FMath::Max(0.f, FMath::Min(FMath::Clamp(Effector.TrimFraction, 0.f, 1.f),
			FMath::Clamp(Effector.BaseThrottle, 0.f, 1.f)));
	}

	/** The travel available in both directions, i.e. the symmetric part. */
	inline float TrimBound(const FTrimEffector& Effector)
	{
		return FMath::Min(TrimBoundMax(Effector), -TrimBoundMin(Effector));
	}

	/**
	 * Hold every bias inside its own valve's travel. Called AFTER the slew,
	 * because the slew is measured from last frame's committed bias and the
	 * bound moves with the collective: a valve slewing toward a smaller bound
	 * could carry a bias up to SlewPerSec*dt outside it - 0.033 at 60 fps,
	 * about 130 N and 100 N*m on the test rover - and the commit clamp then
	 * silently broke both the force-neutral guarantee and the standing-moment
	 * feed-forward for that frame.
	 */
	inline void ClampTrimsToBounds(TArrayView<FTrimEffector> Effectors)
	{
		for (FTrimEffector& Effector : Effectors)
		{
			Effector.Trim = FMath::Clamp(Effector.Trim, TrimBoundMin(Effector), TrimBoundMax(Effector));
		}
	}

	/** Sum of the lift each unit could add per unit throttle (N). The lift scale of a craft. */
	inline float TrimLiftScaleN(TArrayView<const FTrimEffector> Effectors)
	{
		double Total = 0.0;
		for (const FTrimEffector& Effector : Effectors)
		{
			Total += FMath::Abs(Effector.LiftPerUnitThrottle);
		}
		return static_cast<float>(Total);
	}

	/**
	 * Remove the net lift the trims add, so a trim pair is a pure moment.
	 * Jacobian transpose on the FORCE row, restricted to units that still have
	 * travel in the needed direction, and repeated because each pass may push a
	 * unit onto its bound. Lift is linear in the trims and an all-zero bias is
	 * always feasible, so the projected step is a descent direction and the
	 * residual falls every pass.
	 *
	 * If some geometry still cannot make the moment without moving the lift,
	 * the bias is ABANDONED rather than committed. An unwind is worth having
	 * only if it is free of altitude - the pilot cannot fly around an altitude
	 * that moves on its own, and the alternative is the old behaviour, which
	 * leaked lift with an adverse sign: asking for nose-up removed lift.
	 */
	/** Largest number of force axes the null will hold at once. */
	inline constexpr int32 MaxTrimNullAxes = 3;

	/**
	 * Gauss-Jordan solve of a small symmetric system, N <= MaxTrimNullAxes.
	 * Returns false if the matrix is singular, which is what a craft with no
	 * force authority on one of the requested axes looks like.
	 */
	inline bool SolveSmallSystem(double A[MaxTrimNullAxes][MaxTrimNullAxes + 1], int32 N, double OutX[MaxTrimNullAxes])
	{
		for (int32 Col = 0; Col < N; ++Col)
		{
			int32 Pivot = Col;
			for (int32 Row = Col + 1; Row < N; ++Row)
			{
				if (FMath::Abs(A[Row][Col]) > FMath::Abs(A[Pivot][Col]))
				{
					Pivot = Row;
				}
			}
			if (FMath::Abs(A[Pivot][Col]) <= UE_DOUBLE_KINDA_SMALL_NUMBER)
			{
				return false;
			}
			for (int32 K = Col; K <= N; ++K)
			{
				Swap(A[Col][K], A[Pivot][K]);
			}
			for (int32 Row = 0; Row < N; ++Row)
			{
				if (Row == Col)
				{
					continue;
				}
				const double Factor = A[Row][Col] / A[Col][Col];
				for (int32 K = Col; K <= N; ++K)
				{
					A[Row][K] -= Factor * A[Col][K];
				}
			}
		}
		for (int32 Row = 0; Row < N; ++Row)
		{
			OutX[Row] = A[Row][N] / A[Row][Row];
		}
		return true;
	}

	/** Net force the current trims add along one axis (N). Must be zero at commit. */
	inline float NetTrimForceAlongN(TArrayView<const FTrimEffector> Effectors, const FVector& Axis)
	{
		double Total = 0.0;
		for (const FTrimEffector& Effector : Effectors)
		{
			Total += FVector::DotProduct(Effector.ForcePerUnitThrottle, Axis) * Effector.Trim;
		}
		return static_cast<float>(Total);
	}

	/**
	 * Remove the net force the trims add along each of NumAxes directions, so
	 * a trim pair is a pure moment. Jacobian transpose on the FORCE rows,
	 * restricted to units that still have travel in the needed direction, and
	 * repeated because each pass may push a unit onto its bound. Force is
	 * linear in the trims and an all-zero bias is always feasible, so the
	 * projected step is a descent direction and the residual falls every pass.
	 *
	 * Returns false when the geometry cannot make the moment without moving
	 * the force - the caller then either drops an axis or abandons the bias
	 * entirely, because an unwind is only worth having if it is free of
	 * altitude and ground track. The alternative is the old behaviour, which
	 * leaked with an adverse sign: asking for nose-up removed lift.
	 */
	inline bool NullNetTrimForce(TArrayView<FTrimEffector> Effectors, const FVector* Axes, int32 NumAxes)
	{
		NumAxes = FMath::Clamp(NumAxes, 0, MaxTrimNullAxes);
		if (NumAxes == 0)
		{
			return true;
		}
		double Scale = 0.0;
		for (const FTrimEffector& Effector : Effectors)
		{
			for (int32 Axis = 0; Axis < NumAxes; ++Axis)
			{
				Scale += FMath::Abs(FVector::DotProduct(Effector.ForcePerUnitThrottle, Axes[Axis]));
			}
		}
		const double Tolerance = FMath::Max(static_cast<double>(KINDA_SMALL_NUMBER), Scale * 1e-4);

		for (int32 Pass = 0; Pass < 8; ++Pass)
		{
			double Net[MaxTrimNullAxes] = { 0.0, 0.0, 0.0 };
			double Worst = 0.0;
			for (int32 Axis = 0; Axis < NumAxes; ++Axis)
			{
				Net[Axis] = NetTrimForceAlongN(Effectors, Axes[Axis]);
				Worst = FMath::Max(Worst, FMath::Abs(Net[Axis]));
			}
			if (Worst <= Tolerance)
			{
				return true;
			}
			// Only units that can still move the right way carry the correction.
			double A[MaxTrimNullAxes][MaxTrimNullAxes + 1] = {};
			for (const FTrimEffector& Effector : Effectors)
			{
				double Row[MaxTrimNullAxes] = { 0.0, 0.0, 0.0 };
				double Step = 0.0;
				for (int32 Axis = 0; Axis < NumAxes; ++Axis)
				{
					Row[Axis] = FVector::DotProduct(Effector.ForcePerUnitThrottle, Axes[Axis]);
					Step -= Net[Axis] * Row[Axis];
				}
				const bool bFree = (Step > 0.0 && Effector.Trim < TrimBoundMax(Effector))
					|| (Step < 0.0 && Effector.Trim > TrimBoundMin(Effector));
				if (!bFree)
				{
					continue;
				}
				for (int32 R = 0; R < NumAxes; ++R)
				{
					for (int32 C = 0; C < NumAxes; ++C)
					{
						A[R][C] += Row[R] * Row[C];
					}
				}
			}
			double Trace = 0.0;
			for (int32 Axis = 0; Axis < NumAxes; ++Axis)
			{
				Trace += A[Axis][Axis];
			}
			if (Trace <= UE_DOUBLE_KINDA_SMALL_NUMBER)
			{
				break;   // nothing left with travel
			}
			for (int32 Axis = 0; Axis < NumAxes; ++Axis)
			{
				A[Axis][Axis] += Trace * 1e-6;
				A[Axis][NumAxes] = -Net[Axis];
			}
			double Y[MaxTrimNullAxes] = { 0.0, 0.0, 0.0 };
			if (!SolveSmallSystem(A, NumAxes, Y))
			{
				break;
			}
			for (FTrimEffector& Effector : Effectors)
			{
				double Delta = 0.0;
				for (int32 Axis = 0; Axis < NumAxes; ++Axis)
				{
					Delta += FVector::DotProduct(Effector.ForcePerUnitThrottle, Axes[Axis]) * Y[Axis];
				}
				Effector.Trim = FMath::Clamp(static_cast<float>(Effector.Trim + Delta),
					TrimBoundMin(Effector), TrimBoundMax(Effector));
			}
		}

		double Worst = 0.0;
		for (int32 Axis = 0; Axis < NumAxes; ++Axis)
		{
			Worst = FMath::Max(Worst, FMath::Abs(static_cast<double>(NetTrimForceAlongN(Effectors, Axes[Axis]))));
		}
		return Worst <= Tolerance;
	}

	/**
	 * Hold the committed bias force-neutral, dropping axes from the back and
	 * abandoning the bias entirely rather than committing one that pushes the
	 * craft in a direction nobody asked for. This is the guard AFTER the slew
	 * and the bound clamp, both of which can reintroduce a leak the allocator
	 * had already removed.
	 */
	inline void EnforceForceNeutralTrim(TArrayView<FTrimEffector> Effectors, const FVector* Axes, int32 NumAxes)
	{
		for (int32 Count = FMath::Clamp(NumAxes, 0, MaxTrimNullAxes); Count >= 1; --Count)
		{
			if (NullNetTrimForce(Effectors, Axes, Count))
			{
				return;
			}
		}
		for (FTrimEffector& Effector : Effectors)
		{
			Effector.Trim = 0.f;
		}
	}

	/**
	 * Rate-limit the whole trim vector toward the requested one at the valve's
	 * authored slew rate. Deliberately a UNIFORM scale of the step rather than
	 * a per-unit clamp: lift is linear in the trims, so scaling a lift-neutral
	 * step keeps it lift-neutral, while clamping units independently would not.
	 */
	inline void SlewTrimTowardsRequest(TArrayView<FTrimEffector> Effectors, float DeltaSeconds, float SlewPerSec)
	{
		const float MaxStep = FMath::Max(SlewPerSec, 0.f) * FMath::Max(DeltaSeconds, 0.f);
		float Largest = 0.f;
		for (const FTrimEffector& Effector : Effectors)
		{
			Largest = FMath::Max(Largest, FMath::Abs(Effector.Trim - Effector.PreviousTrim));
		}
		if (Largest <= MaxStep || Largest <= KINDA_SMALL_NUMBER)
		{
			return;
		}
		const float Scale = MaxStep / Largest;
		for (FTrimEffector& Effector : Effectors)
		{
			Effector.Trim = Effector.PreviousTrim + (Effector.Trim - Effector.PreviousTrim) * Scale;
		}
	}

	/**
	 * Normal matrix of the moment Jacobian, A = J * J^T (3x3, symmetric), where
	 * row i of J holds every unit's moment about axis i per unit of throttle.
	 * A is singular exactly when the craft's thrust geometry cannot make a
	 * moment about some axis at all - six lift units pointing along the craft's
	 * own up axis make no yaw moment whatsoever - so the solver below has to
	 * cope with that rather than assume it away.
	 */
	inline void TrimNormalMatrix(TArrayView<const FTrimEffector> Effectors, double Out[3][3])
	{
		for (int32 Row = 0; Row < 3; ++Row)
		{
			for (int32 Col = 0; Col < 3; ++Col)
			{
				double Sum = 0.0;
				for (const FTrimEffector& Effector : Effectors)
				{
					Sum += Effector.MomentPerUnitThrottle[Row] * Effector.MomentPerUnitThrottle[Col];
				}
				Out[Row][Col] = Sum;
			}
		}
	}

	/**
	 * ONE MINIMUM-NORM ALLOCATION STEP, the standard control-allocation
	 * pseudo-inverse: solve (J J^T + lambda I) y = Desired, then bias each unit
	 * by (its moment) dot y, clipped to its own symmetric bound. Among all the
	 * bias vectors that make the wanted moment it picks the smallest one, which
	 * is the one that spends the least valve travel.
	 *
	 * This replaced a step that pushed every unit along the single direction of
	 * the desired moment. That one is a gradient step on one scalar, not a
	 * solve, and it converged at a crawl when the axes are coupled: on the test
	 * rover, holding the forward key at hover, twenty passes still left 36 N*m
	 * of the standing moment on the table for the rotors to pay for. The
	 * pseudo-inverse closes the same case to zero in two.
	 *
	 * lambda is a ridge term proportional to the trace, so a rank-deficient
	 * geometry gives a large y on the unreachable axis multiplied by the zero
	 * moment row - no torque, no NaN, no pretending.
	 */
	inline bool AllocateTrimStep(TArrayView<FTrimEffector> Effectors, const FVector& DesiredTorqueNm, double StepScale = 1.0)
	{
		if (Effectors.Num() == 0)
		{
			return false;
		}
		double A[3][3];
		TrimNormalMatrix(Effectors, A);
		const double Trace = FMath::Max(1.0, A[0][0] + A[1][1] + A[2][2]);
		const double Lambda = Trace * 1e-4;
		A[0][0] += Lambda;
		A[1][1] += Lambda;
		A[2][2] += Lambda;

		auto Det3 = [](const double (&M)[3][3])
		{
			return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1])
				- M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
				+ M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
		};
		const double Determinant = Det3(A);
		if (FMath::Abs(Determinant) <= UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			return false;
		}
		double Y[3] = { 0.0, 0.0, 0.0 };
		for (int32 Column = 0; Column < 3; ++Column)
		{
			double M[3][3];
			for (int32 Row = 0; Row < 3; ++Row)
			{
				for (int32 Col = 0; Col < 3; ++Col)
				{
					M[Row][Col] = (Col == Column) ? DesiredTorqueNm[Row] : A[Row][Col];
				}
			}
			Y[Column] = Det3(M) / Determinant;
		}
		for (FTrimEffector& Effector : Effectors)
		{
			const double Wanted = StepScale * (Effector.MomentPerUnitThrottle.X * Y[0]
				+ Effector.MomentPerUnitThrottle.Y * Y[1]
				+ Effector.MomentPerUnitThrottle.Z * Y[2]);
			Effector.Trim = FMath::Clamp(static_cast<float>(Effector.Trim + Wanted),
				TrimBoundMin(Effector), TrimBoundMax(Effector));
		}
		return true;
	}

	/**
	 * Drive the biases toward DesiredTorqueNm, keeping the result free of net
	 * lift, and NEVER leave the craft worse off than doing nothing.
	 *
	 * Two things make the iteration necessary. Each step clips units onto their
	 * bounds, and the lift null then gives part of the moment back, so one step
	 * is not the answer even for reachable geometry. And when the request is
	 * NOT reachable - the valves are near a stop, or the axes are coupled the
	 * wrong way - a clipped pseudo-inverse step can land somewhere worse than
	 * the untrimmed craft: on the test rover with the lift valve shut, trimming
	 * the forward pair's 41 N*m of yaw cost 38 N*m of extra pitch. So every
	 * step is accepted only if it reduces the residual, with the step halved a
	 * few times before it is given up on. Starting from no bias at all, that
	 * makes "the trim never hurts" a property rather than a hope.
	 *
	 * Result on the shipped test rover: the residual standing moment is ZERO on
	 * all three axes at every collective from 0.05 to the reserved ceiling,
	 * with or without full forward thrust. Only a fully shut valve has no
	 * authority left, and then the residual is exactly the untrimmed moment and
	 * the visor says how many seconds of rotor it costs.
	 */
	inline bool AllocateTrimWithNull(TArrayView<FTrimEffector> Effectors, const FVector& DesiredTorqueNm,
		const FVector* NullAxes, int32 NumNullAxes, int32 MaxPasses)
	{
		TArray<float, TInlineAllocator<16>> Saved;
		Saved.SetNumUninitialized(Effectors.Num());

		for (int32 Pass = 0; Pass < MaxPasses; ++Pass)
		{
			const double BestResidual = (DesiredTorqueNm - DeliveredTrimTorqueNm(Effectors)).Size();
			if (BestResidual <= 1e-4)
			{
				return true;
			}
			for (int32 Index = 0; Index < Effectors.Num(); ++Index)
			{
				Saved[Index] = Effectors[Index].Trim;
			}
			bool bImproved = false;
			for (const double StepScale : { 1.0, 0.5, 0.25, 0.125 })
			{
				for (int32 Index = 0; Index < Effectors.Num(); ++Index)
				{
					Effectors[Index].Trim = Saved[Index];
				}
				if (!AllocateTrimStep(Effectors, DesiredTorqueNm - DeliveredTrimTorqueNm(Effectors), StepScale))
				{
					break;
				}
				if (!NullNetTrimForce(Effectors, NullAxes, NumNullAxes))
				{
					// This axis set is not achievable from here. Hand the
					// decision back rather than committing a bias that moves
					// the craft in a direction nobody asked for.
					for (int32 Index = 0; Index < Effectors.Num(); ++Index)
					{
						Effectors[Index].Trim = Saved[Index];
					}
					return false;
				}
				if ((DesiredTorqueNm - DeliveredTrimTorqueNm(Effectors)).Size() < BestResidual - 1e-6)
				{
					bImproved = true;
					break;
				}
			}
			if (!bImproved)
			{
				for (int32 Index = 0; Index < Effectors.Num(); ++Index)
				{
					Effectors[Index].Trim = Saved[Index];
				}
				return true;
			}
		}
		return true;
	}

	/**
	 * Drive the biases toward DesiredTorqueNm while adding no net force along
	 * the given axes, and NEVER leave the craft worse off than doing nothing.
	 *
	 * Two things make the iteration necessary. Each step clips units onto
	 * their bounds, and the force null then gives part of the moment back, so
	 * one step is not the answer even for reachable geometry. And when the
	 * request is NOT reachable - the valves are near a stop, or the axes are
	 * coupled the wrong way - a clipped pseudo-inverse step can land somewhere
	 * worse than the untrimmed craft: on the test rover with the lift valve
	 * shut, trimming the forward pair's 41 N*m of yaw cost 38 N*m of extra
	 * pitch. So every step is accepted only if it reduces the residual, with
	 * the step halved a few times before it is given up on. Starting from no
	 * bias at all, that makes "the trim never hurts" a property, not a hope.
	 *
	 * THE AXES ARE TRIED IN ORDER AND DROPPED FROM THE BACK. Axis 0 is the
	 * craft's lift axis and is never dropped, because an altitude that moves
	 * on its own is unflyable. The later axes are ground track, and a craft
	 * whose geometry cannot make a moment without some sideways force gets the
	 * moment and a few tens of newtons of side force rather than no moment at
	 * all - which on the test rover is the difference between cancelling its
	 * standing pitch moment - measured 485 N*m at the hover collective with no
	 * key held, 241 N*m with the forward key held, 334 N*m at the reserved
	 * ceiling - and paying for it out of the rotors.
	 */
	/**
	 * MaxPasses 24, measured rather than guessed. Each pass is one clipped
	 * pseudo-inverse step followed by the force null, and the null gives part of
	 * the moment back, so convergence on a COUPLED request is geometric rather
	 * than immediate. On the shipped rover holding a full-rate turn at the hover
	 * collective - request (-104, -490, +74) N*m, all three axes at once - eight
	 * passes delivered 73.5 percent of the yaw term and left 19.5 N*m on the
	 * table; sixteen reached 93.1 percent, and twenty-four reached 99.0 percent
	 * (0.75 N*m). 19.5 N*m is 19.5 N*m*s of rotor momentum EVERY SECOND, which
	 * is a yaw axis that fills after about 80 s of continuous turning; 0.75 N*m
	 * is 2100 s. The loop self-terminates the moment a step stops improving
	 * (measured at pass 25 on that request), so the extra passes cost nothing
	 * on a request that converges early - a single-axis ask still exits in two.
	 */
	inline void AllocateForceNeutralTrim(TArrayView<FTrimEffector> Effectors, const FVector& DesiredTorqueNm,
		const FVector* NullAxes, int32 NumNullAxes, int32 MaxPasses = 24)
	{
		if (Effectors.Num() == 0 || DesiredTorqueNm.IsNearlyZero() || NumNullAxes <= 0)
		{
			return;
		}
		for (int32 Axes = FMath::Min(NumNullAxes, MaxTrimNullAxes); Axes >= 1; --Axes)
		{
			for (FTrimEffector& Effector : Effectors)
			{
				Effector.Trim = 0.f;
			}
			if (AllocateTrimWithNull(Effectors, DesiredTorqueNm, NullAxes, Axes, MaxPasses))
			{
				return;
			}
		}
		// Not even lift-neutral: commit nothing.
		for (FTrimEffector& Effector : Effectors)
		{
			Effector.Trim = 0.f;
		}
	}

}
