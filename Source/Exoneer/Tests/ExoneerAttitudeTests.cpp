// Copyright Exoneer contributors.
//
// Unit tests for the pure attitude-control library (Vehicles/ExoneerAttitude.h).
// These close the loop numerically on the SAME hull the owner flies - the 6x6
// test rover, whose inertia about the centre of mass is Ixx 308, Iyy 1355,
// Izz 1544 kg*m^2 - and assert the properties the flight model is supposed to
// have, rather than reproducing the code's own arithmetic.
//
// WHAT THE FLIGHT LOOP ACTUALLY RUNS is the RATE law: the stick asks for a body
// rate, the triad drives the hull onto it, and a released stick is a pure rate
// null. RunRateAxis is that loop. The attitude-HOLD half of the library (the
// reference, its leash, the Kp term) is not engaged by the vehicle; it stays
// here under test, with its windup leash exercised on every axis, so it can be
// reintroduced deliberately rather than rebuilt from scratch.
//
// Headless run:
//   UnrealEditor-Cmd.exe <uproject> -ExecCmds="Automation RunTests Exoneer.Attitude; Quit"
//     -TestExit="Automation Test Queue Empty" -unattended -nullrhi -nosplash -nop4 -log

#include "Misc/AutomationTest.h"
#include "Vehicles/ExoneerAttitude.h"
#include "Data/VehicleBlockDefinitionDataAsset.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	using namespace ExoneerAttitude;

	/** Inertia of the 6x6 test rover about its centre of mass (kg*m^2). */
	const FVector RoverInertia(308.3f, 1354.8f, 1543.8f);

	/** The rover's two authored triads. */
	constexpr float RoverRatedTorqueNm = 4000.f;      // 2 x 2000 N*m per axis
	constexpr float RoverCapacityNms = 1600.f;        // 2 x 800 N*m*s per axis

	/** Authored on DA_Block_Gyro. Mirrored here so a retune has to touch the tests. */
	constexpr float RoverSettleTimeSeconds = 0.25f;
	constexpr float RoverRateCeilingDegPerSec = 20.f;
	constexpr float RoverCommandMomentumFraction = 0.5f;
	constexpr float RoverDumpOnsetFraction = 0.8f;
	constexpr float RoverDumpReleaseFraction = 0.4f;
	constexpr float RoverDumpRatePerSec = 0.35f;
	/**
	 * How long a full-deflection turn is paid for out of the ROTORS ALONE (s).
	 * Holding a rate costs D*I*w every second, so this is the term the previous
	 * rate limit left out entirely - and leaving it out is what killed the yaw
	 * axis after one continuous 202 degree turn.
	 */
	constexpr float RoverSustainedTurnSeconds = 4.f;
	constexpr float RoverLevelRateDegPerSec = 20.f;
	/**
	 * Authored bank ceiling (deg). Chosen against the craft: the rover's
	 * reserved ceiling holds weight out to acos(18120 / (23738 x 0.90)) = 32.0
	 * degrees, so 30 keeps every commandable attitude one it can hold its own
	 * weight at.
	 */
	constexpr float RoverBankCeilingDeg = 30.f;
	/** Angle past which the reserved ceiling can no longer hold weight (deg). */
	constexpr float RoverHoldWeightLimitDeg = 32.0f;

	FLoopParams RoverLoop(float HullDamping = 0.15f)
	{
		FLoopParams Loop;
		Loop.InertiaKgM2 = RoverInertia;
		Loop.SettleTimeSeconds = RoverSettleTimeSeconds;
		Loop.DampingRatio = 1.f;
		Loop.HullAngularDamping = HullDamping;
		return Loop;
	}

	struct FAxisRun
	{
		TArray<float> Angle;
		TArray<float> Rate;
		float PeakTorque = 0.f;
	};

	/**
	 * THE LOOP THE PILOT FLIES. One axis of a rigid body under the rate law:
	 * torque = Kd * (commanded rate - actual rate), clamped at the installed
	 * rating, integrated explicitly at the frame rate, plus the body's own
	 * angular damping. No reference, no spring, so nothing can wind up.
	 */
	FAxisRun RunRateAxis(int32 AxisIndex, const FLoopParams& Loop, float CommandRateRadS,
		float CommandSeconds, float TotalSeconds, float Fps, float RatedTorqueNm, float StartRateRadS = 0.f)
	{
		const float Dt = 1.f / Fps;
		const float Inertia = Loop.InertiaKgM2[AxisIndex];
		const float Kd = RateGain(Loop)[AxisIndex];

		float Angle = 0.f;
		float Rate = StartRateRadS;
		FAxisRun Run;
		const int32 Steps = FMath::RoundToInt(TotalSeconds / Dt);
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			const float Command = (Step * Dt < CommandSeconds) ? CommandRateRadS : 0.f;
			float Torque = Kd * (Command - Rate);
			Torque = FMath::Clamp(Torque, -RatedTorqueNm, RatedTorqueNm);
			Run.PeakTorque = FMath::Max(Run.PeakTorque, FMath::Abs(Torque));

			Rate += (Torque / Inertia - Loop.HullAngularDamping * Rate) * Dt;
			Angle += Rate * Dt;
			Run.Angle.Add(Angle);
			Run.Rate.Add(Rate);
		}
		return Run;
	}

	/**
	 * The attitude-HOLD loop, kept under test but NOT engaged by the vehicle.
	 * The hull starts displaced from a reference at zero and has to come back.
	 *
	 * Note the leash: the reference is pulled to within MaxHoldableErrorRad of
	 * the hull every step, so a disturbance larger than the leash permanently
	 * discards part of the target. Callers must keep StartAngleRad inside the
	 * smallest axis leash or they are testing the clamp, not the loop.
	 */
	FAxisRun RunHoldAxis(int32 AxisIndex, const FLoopParams& Loop, float TotalSeconds,
		float Fps, float RatedTorqueNm, float StartAngleRad)
	{
		const float Dt = 1.f / Fps;
		const float Inertia = Loop.InertiaKgM2[AxisIndex];
		const float Kp = PositionGain(Loop)[AxisIndex];
		const float Kd = RateGain(Loop)[AxisIndex];
		const float MaxError = MaxHoldableErrorRad(Loop, RatedTorqueNm)[AxisIndex];

		float Angle = StartAngleRad;
		float Rate = 0.f;
		float Reference = 0.f;
		FAxisRun Run;
		const int32 Steps = FMath::RoundToInt(TotalSeconds / Dt);
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			Reference = FMath::Clamp(Reference, Angle - MaxError, Angle + MaxError);

			float Torque = Kp * (Reference - Angle) + Kd * (0.f - Rate);
			Torque = FMath::Clamp(Torque, -RatedTorqueNm, RatedTorqueNm);
			Run.PeakTorque = FMath::Max(Run.PeakTorque, FMath::Abs(Torque));

			Rate += (Torque / Inertia - Loop.HullAngularDamping * Rate) * Dt;
			Angle += Rate * Dt;
			Run.Angle.Add(Angle);
			Run.Rate.Add(Rate);
		}
		return Run;
	}
}

/**
 * Gains follow the hull, not a hand-picked stiffness. Kp = I/T^2 and
 * Kd = (2*zeta/T - D_hull)*I on every axis, so the roll axis is not 4.4x
 * stiffer than the pitch axis just because it happens to be lighter.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerAttitudeGainsScaleWithInertia, "Exoneer.Attitude.GainsScaleWithInertia",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerAttitudeGainsScaleWithInertia::RunTest(const FString& Parameters)
{
	const FLoopParams Loop = RoverLoop();
	const FVector Kp = PositionGain(Loop);
	const FVector Kd = RateGain(Loop);

	const float Wn = 1.f / Loop.SettleTimeSeconds;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		TestEqual(FString::Printf(TEXT("Kp[%d] = wn^2 * I"), Axis),
			static_cast<float>(Kp[Axis]), Wn * Wn * static_cast<float>(RoverInertia[Axis]), 1e-2f);
		TestEqual(FString::Printf(TEXT("Kd[%d] = (2*zeta*wn - D) * I"), Axis),
			static_cast<float>(Kd[Axis]),
			(2.f * Wn - Loop.HullAngularDamping) * static_cast<float>(RoverInertia[Axis]), 1e-2f);
	}

	// The whole point: the rate-loop bandwidth Kd/I is the SAME on every axis.
	// The old law used a flat gain, which made roll 4.4x stiffer than yaw
	// purely because the hull is narrow.
	const float RollBandwidth = static_cast<float>(Kd.X / RoverInertia.X);
	const float YawBandwidth = static_cast<float>(Kd.Z / RoverInertia.Z);
	TestEqual(TEXT("roll and yaw rate-loop bandwidth match"), RollBandwidth, YawBandwidth, 1e-3f);

	// The authored settle time has to leave a rate loop the pilot reads as
	// crisp: a released stick must null the body rate in a fraction of a
	// second, not in a second and a half. Tau = I / (Kd + I*D).
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const float Tau = static_cast<float>(RoverInertia[Axis])
			/ (static_cast<float>(Kd[Axis]) + static_cast<float>(RoverInertia[Axis]) * Loop.HullAngularDamping);
		TestTrue(FString::Printf(TEXT("axis %d rate-null time constant %.3f s is under 0.2 s"), Axis, Tau),
			Tau < 0.2f);
	}

	// A hull already damped past the target asks the triad for no extra damping.
	const FLoopParams Overdamped = RoverLoop(10.f);
	TestTrue(TEXT("Kd floors at zero when the hull is already overdamped"), RateGain(Overdamped).IsNearlyZero());
	return true;
}

/**
 * ATTITUDE HOLD, library coverage only - the vehicle does not engage it. The
 * hull is knocked off its reference and has to come back from rest with NO
 * overshoot at all (bound: 1 percent of the disturbance) on every axis and at
 * every frame rate a player will see.
 *
 * The disturbance is deliberately inside the SMALLEST axis leash (yaw, 9.3 deg
 * at the authored settle time). A larger one tests the leash clamp instead.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerAttitudeHoldDoesNotOvershoot, "Exoneer.Attitude.HoldDoesNotOvershoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerAttitudeHoldDoesNotOvershoot::RunTest(const FString& Parameters)
{
	const FLoopParams Loop = RoverLoop();
	const FVector Leash = MaxHoldableErrorRad(Loop, RoverRatedTorqueNm);
	const float StepRad = 0.15f;                 // 8.6 deg, inside all three leashes
	constexpr float OvershootTolerance = 0.01f;  // 1 percent of the disturbance
	const float RateTolerance = FMath::DegreesToRadians(0.5f);

	TestTrue(TEXT("the test step sits inside every axis leash"),
		StepRad < FMath::Min3(Leash.X, Leash.Y, Leash.Z));

	for (const float Fps : { 120.f, 60.f, 45.f, 30.f, 20.f })
	{
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const FAxisRun Run = RunHoldAxis(Axis, Loop, 8.f, Fps, RoverRatedTorqueNm, -StepRad);
			float Peak = 0.f;
			for (const float Angle : Run.Angle)
			{
				Peak = FMath::Max(Peak, Angle);
			}
			TestTrue(FString::Printf(TEXT("axis %d at %.0f fps returns to the reference, %.4f rad left"),
					Axis, Fps, FMath::Abs(Run.Angle.Last())),
				FMath::Abs(Run.Angle.Last()) < StepRad * 0.02f);
			TestTrue(FString::Printf(TEXT("axis %d at %.0f fps overshoots %.3f of the step, bound %.3f"),
					Axis, Fps, Peak / StepRad, OvershootTolerance),
				Peak <= StepRad * OvershootTolerance);
			TestTrue(FString::Printf(TEXT("axis %d at %.0f fps residual rate %.4f rad/s"), Axis, Fps, Run.Rate.Last()),
				FMath::Abs(Run.Rate.Last()) < RateTolerance);
		}
	}
	return true;
}

/**
 * THE LAW THE PILOT FLIES. The stick is a rate command and a released stick is
 * a rate NULL. Three properties, all of them things the owner reported missing:
 *
 *   - the commanded rate is actually reached, so the control is proportional
 *     rather than a torque switch;
 *   - letting go stops the rotation, and stops it quickly;
 *   - it never rings. The rate may not reverse at all under this law, and the
 *     attitude must approach its final value monotonically.
 *
 * A craft with angular momentum still coasts past the attitude it held when
 * the stick was released - that is physics, and without an attitude reference
 * there is nothing to pull it back. What must not happen is oscillation.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerAttitudeRateCommandDoesNotRing, "Exoneer.Attitude.RateCommandDoesNotRing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerAttitudeRateCommandDoesNotRing::RunTest(const FString& Parameters)
{
	const FLoopParams Loop = RoverLoop();
	const FVector RateLimit = MaxCommandRateRadS(Loop, RoverCapacityNms,
		RoverCommandMomentumFraction, FMath::DegreesToRadians(RoverRateCeilingDegPerSec),
		RoverSustainedTurnSeconds);
	const float RateTolerance = FMath::DegreesToRadians(0.5f);

	for (const float Fps : { 120.f, 60.f, 45.f, 30.f, 20.f, 10.f })
	{
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const FAxisRun Run = RunRateAxis(Axis, Loop, RateLimit[Axis], 2.f, 6.f, Fps, RoverRatedTorqueNm);

			// Tracking: the held rate is reached to within a few percent. The
			// small droop is the hull's own damping, which the gain subtracts.
			float PeakRate = 0.f;
			int32 Reversals = 0;
			float PeakAngle = 0.f;
			for (int32 Index = 1; Index < Run.Rate.Num(); ++Index)
			{
				PeakRate = FMath::Max(PeakRate, Run.Rate[Index]);
				PeakAngle = FMath::Max(PeakAngle, Run.Angle[Index]);
				if (Run.Rate[Index] * Run.Rate[Index - 1] < 0.f
					&& FMath::Abs(Run.Rate[Index]) > RateTolerance)
				{
					++Reversals;
				}
			}
			TestTrue(FString::Printf(TEXT("axis %d at %.0f fps tracks the command: %.4f of %.4f rad/s"),
					Axis, Fps, PeakRate, RateLimit[Axis]),
				PeakRate > RateLimit[Axis] * 0.95f && PeakRate <= RateLimit[Axis] + 1e-3f);

			// Release: the rate nulls, and the attitude never comes back past
			// where it got to - a rate null cannot ring, by construction.
			TestTrue(FString::Printf(TEXT("axis %d at %.0f fps never reverses rate (%d reversals)"), Axis, Fps, Reversals),
				Reversals == 0);
			TestEqual(FString::Printf(TEXT("axis %d at %.0f fps attitude settles at its peak"), Axis, Fps),
				Run.Angle.Last(), PeakAngle, 1e-4f);
			TestTrue(FString::Printf(TEXT("axis %d at %.0f fps residual rate %.5f rad/s"), Axis, Fps, Run.Rate.Last()),
				FMath::Abs(Run.Rate.Last()) < RateTolerance);
			TestTrue(FString::Printf(TEXT("axis %d peak torque %.0f N*m stays inside the rating"), Axis, Run.PeakTorque),
				Run.PeakTorque <= RoverRatedTorqueNm + 1e-2f);
		}
	}

	// The property in the owner's own words: let go and it stops. From a
	// half-rad-per-second tumble with no command at all, the rate must be
	// essentially gone inside half a second on every axis.
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const FAxisRun Run = RunRateAxis(Axis, Loop, 0.f, 0.f, 0.5f, 60.f, RoverRatedTorqueNm, 0.5f);
		TestTrue(FString::Printf(TEXT("axis %d arrests a 0.5 rad/s tumble in 0.5 s (%.4f left)"), Axis, Run.Rate.Last()),
			FMath::Abs(Run.Rate.Last()) < FMath::DegreesToRadians(1.f));
	}
	return true;
}

/**
 * The discrete loop must be stable at any frame rate a player will see,
 * INCLUDING the bad ones. The explicit-Euler stability limit for the rate loop
 * is dt * Kd/I < 2; the old flat-gain law sat at 1.30 on the roll axis at 20
 * fps and diverged below about 23 fps, which is the ringing the owner reported.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerAttitudeDiscreteStability, "Exoneer.Attitude.DiscreteStability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerAttitudeDiscreteStability::RunTest(const FString& Parameters)
{
	const FLoopParams Loop = RoverLoop();
	const FVector Kd = RateGain(Loop);

	// 10 fps is worse than anything playable, and it is where the authored
	// settle time has to still be provably stable.
	for (const float Fps : { 60.f, 30.f, 20.f, 10.f })
	{
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const float LoopGain = (1.f / Fps) * static_cast<float>(Kd[Axis]) / static_cast<float>(RoverInertia[Axis]);
			TestTrue(FString::Printf(TEXT("axis %d discrete rate-loop gain %.3f < 1 at %.0f fps"), Axis, LoopGain, Fps),
				LoopGain < 1.f);
		}
	}

	// And the closed loop must actually decay a tumble at those frame rates,
	// monotonically, rather than merely staying bounded.
	for (const float Fps : { 60.f, 30.f, 20.f, 10.f })
	{
		const FAxisRun Run = RunRateAxis(0, Loop, 0.f, 0.f, 2.f, Fps, RoverRatedTorqueNm, 1.f);
		bool bMonotone = true;
		for (int32 Index = 1; Index < Run.Rate.Num(); ++Index)
		{
			bMonotone = bMonotone && Run.Rate[Index] <= Run.Rate[Index - 1] + 1e-6f && Run.Rate[Index] >= -1e-6f;
		}
		TestTrue(FString::Printf(TEXT("roll rate decays monotonically at %.0f fps"), Fps), bMonotone);
		TestTrue(FString::Printf(TEXT("roll tumble is gone at %.0f fps (%.5f rad/s left)"), Fps, Run.Rate.Last()),
			FMath::Abs(Run.Rate.Last()) < FMath::DegreesToRadians(0.5f));
	}
	return true;
}

/**
 * The gyroscopic cross term -w x h transports no power: it is perpendicular to
 * w by construction, so dot(tau, w) is identically zero. If this ever fails the
 * term is injecting rotational energy, which is the failure mode that made the
 * old oversized momentum store diverge.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerAttitudeCrossTermIsPowerNeutral, "Exoneer.Attitude.CrossTermIsPowerNeutral",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerAttitudeCrossTermIsPowerNeutral::RunTest(const FString& Parameters)
{
	FRandomStream Random(20260904);
	for (int32 Trial = 0; Trial < 200; ++Trial)
	{
		const FVector Rate = Random.VRand() * Random.FRandRange(0.f, 5.f);
		const FVector Momentum = Random.VRand() * Random.FRandRange(0.f, RoverCapacityNms);
		const FVector Torque = GyroscopicTorque(Rate, Momentum);

		const float Power = FVector::DotProduct(Torque, Rate);
		const float Scale = FMath::Max(1.f, Rate.Size() * Momentum.Size());
		TestTrue(FString::Printf(TEXT("trial %d power %.6f is zero (scale %.1f)"), Trial, Power, Scale),
			FMath::Abs(Power) < 1e-3f * Scale);

		// It also does no work on the rotors: perpendicular to h as well.
		TestTrue(FString::Printf(TEXT("trial %d cross term perpendicular to h"), Trial),
			FMath::Abs(FVector::DotProduct(Torque, Momentum)) < 1e-3f * Scale);
	}

	// A vehicle with the authored envelope must not be dominated by this term.
	// At half a rad/s the whole store makes 800 N*m against a 4000 N*m rating -
	// clearly felt, never in charge. The old 20,000 N*m*s store made 10,000.
	const FVector Cross = GyroscopicTorque(FVector(0.5f, 0.f, 0.f), FVector(0.f, RoverCapacityNms, 0.f));
	TestTrue(TEXT("cross torque at 0.5 rad/s stays under the installed rating"), Cross.Size() < RoverRatedTorqueNm);
	return true;
}

/**
 * Saturation is per rotor and one-directional: a wound rotor stops absorbing
 * momentum in the winding direction and keeps full authority the other way,
 * which is what lets a saturated triad recover the moment the pilot steers
 * back. Momentum never disappears on its own.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerAttitudeSaturation, "Exoneer.Attitude.Saturation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerAttitudeSaturation::RunTest(const FString& Parameters)
{
	constexpr float Capacity = 800.f;
	constexpr float Rated = 2000.f;

	// Below capacity nothing is cut, on any axis, in any direction.
	const FVector Command(Rated, -Rated, Rated);
	TestEqual(TEXT("unsaturated command passes through"),
		ApplySaturation(Command, FVector(100.f, -200.f, 0.f), Capacity), Command);

	// A rotor at +capacity winds further on a NEGATIVE torque (the rotor winds
	// opposite the torque it delivers), so that axis is cut and only that axis.
	const FVector Saturated(Capacity, 0.f, 0.f);
	const FVector Winding = ApplySaturation(FVector(-Rated, Rated, -Rated), Saturated, Capacity);
	TestEqual(TEXT("winding axis is cut"), static_cast<float>(Winding.X), 0.f);
	TestEqual(TEXT("second axis untouched"), static_cast<float>(Winding.Y), Rated);
	TestEqual(TEXT("third axis untouched"), static_cast<float>(Winding.Z), -Rated);

	// The unwinding direction still has full authority.
	TestEqual(TEXT("unwinding direction keeps full torque"),
		static_cast<float>(ApplySaturation(FVector(Rated, 0.f, 0.f), Saturated, Capacity).X), Rated);

	// The readout matches the worst axis.
	TestEqual(TEXT("saturation fraction is the worst axis"),
		SaturationFraction(FVector(200.f, -600.f, 100.f), Capacity), 0.75f, 1e-4f);
	TestEqual(TEXT("no capacity reads as zero, never a divide by zero"),
		SaturationFraction(FVector(200.f, 0.f, 0.f), 0.f), 0.f);

	// Integrating a held command against the envelope: a full-rating command on
	// one axis must saturate in exactly Capacity/Rated seconds and then stop
	// producing torque, so the momentum store cannot run past its limit.
	FVector Stored = FVector::ZeroVector;
	constexpr float Dt = 1.f / 60.f;
	float SaturatedAt = -1.f;
	for (int32 Step = 0; Step < 600; ++Step)
	{
		const FVector Applied = ApplySaturation(FVector(Rated, 0.f, 0.f), Stored, Capacity);
		if (Applied.X == 0.0 && SaturatedAt < 0.f)
		{
			SaturatedAt = Step * Dt;
		}
		Stored -= Applied * Dt;
		Stored = Stored.BoundToCube(Capacity);
	}
	TestTrue(TEXT("held full-rating command saturates one axis"), SaturatedAt > 0.f);
	TestEqual(TEXT("saturation time is capacity / rating"), SaturatedAt, Capacity / Rated, 2.f * Dt);
	TestEqual(TEXT("stored momentum stops at the envelope"), static_cast<float>(FMath::Abs(Stored.X)), Capacity, 1e-2f);
	return true;
}

/**
 * The commanded rate is limited by the momentum the rotors can trade for it:
 * reaching rate w on an axis costs I*w. That makes a heavier hull turn slower
 * and a second gyro make the SAME hull turn faster, which is the progression
 * the equipment is supposed to buy.
 *
 * It must also leave the three axes AGREEING. A comfort ceiling the light axis
 * reaches while the heavy ones are momentum-capped at half of it gives the
 * pilot three different craft to fly at once, which is a large part of what
 * "almost uncontrollable" meant.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerAttitudeCommandRateIsMomentumBudget, "Exoneer.Attitude.CommandRateIsMomentumBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerAttitudeCommandRateIsMomentumBudget::RunTest(const FString& Parameters)
{
	const FLoopParams Loop = RoverLoop();
	const float Ceiling = FMath::DegreesToRadians(RoverRateCeilingDegPerSec);

	const FVector OneGyro = MaxCommandRateRadS(Loop, 800.f, RoverCommandMomentumFraction, Ceiling,
		RoverSustainedTurnSeconds);
	const FVector TwoGyros = MaxCommandRateRadS(Loop, RoverCapacityNms, RoverCommandMomentumFraction,
		Ceiling, RoverSustainedTurnSeconds);

	// Progression: a second triad raises every momentum-limited axis and never
	// lowers one.
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		TestTrue(FString::Printf(TEXT("axis %d never gets slower with a second gyro"), Axis),
			TwoGyros[Axis] >= OneGyro[Axis] - 1e-6f);
	}
	TestTrue(TEXT("one gyro leaves pitch momentum-limited below the ceiling"), OneGyro.Y < Ceiling - 1e-3f);
	TestTrue(TEXT("one gyro leaves yaw momentum-limited below the ceiling"), OneGyro.Z < Ceiling - 1e-3f);
	TestEqual(TEXT("a second gyro doubles the yaw rate while it is still momentum-limited"),
		static_cast<float>(TwoGyros.Z), static_cast<float>(OneGyro.Z) * 2.f, 1e-4f);

	// Consistency on the authored craft: the widest and narrowest axis rates
	// must sit within 10 percent of each other, so one response models all
	// three. At a 60 deg/s ceiling this spread was 2x.
	const float Widest = static_cast<float>(FMath::Max3(TwoGyros.X, TwoGyros.Y, TwoGyros.Z));
	const float Narrowest = static_cast<float>(FMath::Min3(TwoGyros.X, TwoGyros.Y, TwoGyros.Z));
	TestTrue(FString::Printf(TEXT("axis rates agree: %.1f to %.1f deg/s"),
			FMath::RadiansToDegrees(Narrowest), FMath::RadiansToDegrees(Widest)),
		Widest <= Narrowest * 1.1f);

	// B7. THE BUDGET PAYS FOR THE HOLD, NOT JUST THE SPIN-UP, and this is the
	// arithmetic the previous limit skipped entirely.
	//
	// Reaching rate w costs I*w of rotor momentum. STAYING at w costs a
	// further D*I*w every second, for ever, because a rigid body with angular
	// damping needs that torque just to hold still at speed. Sizing the limit
	// on I*w alone said the yaw axis could hold 30 deg/s; the truth was that
	// the rotor also had to find 120 N*m*s per second and hit its stop after
	// 202 degrees - a single mouse hold. The limit is now
	// Fraction*Capacity / (I*(1 + D*T)).
	const float Budget = RoverCapacityNms * RoverCommandMomentumFraction;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const float SpinUp = static_cast<float>(RoverInertia[Axis] * TwoGyros[Axis]);
		const float HoldPerSec = static_cast<float>(Loop.HullAngularDamping) * SpinUp;
		const TCHAR* Names[3] = { TEXT("roll"), TEXT("pitch"), TEXT("yaw") };
		AddInfo(FString::Printf(
			TEXT("%s: %.1f deg/s costs %.0f N*m*s to reach and %.0f N*m*s per second to hold, ")
			TEXT("so %.0f s of it fits in the %.0f N*m*s budget"),
			Names[Axis], FMath::RadiansToDegrees(TwoGyros[Axis]), SpinUp, HoldPerSec,
			RoverSustainedTurnSeconds, Budget));
		TestTrue(FString::Printf(TEXT("%s: spin-up plus %.0f s of hold fits the budget (%.0f of %.0f N*m*s)"),
				Names[Axis], RoverSustainedTurnSeconds,
				SpinUp + HoldPerSec * RoverSustainedTurnSeconds, Budget),
			SpinUp + HoldPerSec * RoverSustainedTurnSeconds <= Budget * 1.001f);
	}

	// AND THE OLD SIZING FAILS THAT SAME TEST, or the fix is not measuring
	// anything. Zero seconds of budgeted hold is exactly the previous formula.
	{
		const FVector Old = MaxCommandRateRadS(Loop, RoverCapacityNms, RoverCommandMomentumFraction,
			Ceiling, 0.f);
		const float OldSpinUp = static_cast<float>(RoverInertia.Z * Old.Z);
		const float OldHold = static_cast<float>(Loop.HullAngularDamping) * OldSpinUp;
		const float SecondsToOnset = (RoverCapacityNms * RoverDumpOnsetFraction - OldSpinUp)
			/ FMath::Max(OldHold, 1.f);
		AddInfo(FString::Printf(
			TEXT("the previous sizing allowed %.1f deg/s of yaw: %.0f N*m*s to reach, %.0f N*m*s per second ")
			TEXT("to hold, so the dump onset arrives after %.1f s and %.0f degrees of one continuous turn"),
			FMath::RadiansToDegrees(Old.Z), OldSpinUp, OldHold, SecondsToOnset,
			FMath::RadiansToDegrees(static_cast<float>(Old.Z)) * SecondsToOnset));
		TestTrue(FString::Printf(TEXT("the previous sizing overspends the budget in %.0f s of held yaw"),
				RoverSustainedTurnSeconds),
			OldSpinUp + OldHold * RoverSustainedTurnSeconds > Budget);
		TestTrue(FString::Printf(TEXT("and the corrected limit is genuinely slower: %.1f against %.1f deg/s"),
				FMath::RadiansToDegrees(TwoGyros.Z), FMath::RadiansToDegrees(Old.Z)),
			TwoGyros.Z < Old.Z);
	}

	// No gyros, no rate command at all - the honest "no block, no authority".
	TestTrue(TEXT("no momentum capacity means no commandable rate"),
		MaxCommandRateRadS(Loop, 0.f, RoverCommandMomentumFraction, Ceiling,
			RoverSustainedTurnSeconds).IsNearlyZero());

	// THE LIMIT IS RESOLVED ABOUT THE AXIS IT IS APPLIED TO, so a rotated
	// cockpit gets the right number. Indexing a per-body-axis vector by axis
	// NUMBER and then using it about a cockpit axis is only right when the
	// cockpit is mounted square - on a seat rotated 90 degrees in yaw the pitch
	// ask was being scaled by the ROLL axis budget, which on this hull is 4.4x
	// too fast.
	{
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const FVector Unit = (Axis == 0) ? FVector::XAxisVector
				: ((Axis == 1) ? FVector::YAxisVector : FVector::ZAxisVector);
			TestEqual(FString::Printf(TEXT("axis %d agrees with the per-axis vector"), Axis),
				MaxCommandRateAboutAxisRadS(Loop, Unit, RoverCapacityNms, RoverCommandMomentumFraction,
					Ceiling, RoverSustainedTurnSeconds),
				static_cast<float>(TwoGyros[Axis]), 1e-5f);
		}
		// A seat yawed 90 degrees: its "right" axis is the hull's X, so the
		// pitch command must be sized by the ROLL inertia - and the helper
		// gets that from the geometry instead of from an index.
		const float Rotated = MaxCommandRateAboutAxisRadS(Loop, FVector::XAxisVector, RoverCapacityNms,
			RoverCommandMomentumFraction, Ceiling, RoverSustainedTurnSeconds);
		TestEqual(TEXT("a yawed cockpit's pitch axis is sized by the inertia it really turns about"),
			Rotated, static_cast<float>(TwoGyros.X), 1e-5f);
		// And a diagonal axis lands between its neighbours, which an indexed
		// lookup cannot express at all.
		const FVector Diagonal = FVector(1.f, 1.f, 0.f).GetSafeNormal();
		const float Between = MaxCommandRateAboutAxisRadS(Loop, Diagonal, RoverCapacityNms,
			RoverCommandMomentumFraction, Ceiling, RoverSustainedTurnSeconds);
		TestEqual(TEXT("the effective inertia about a diagonal is u.Iu"),
			EffectiveInertiaAboutAxis(Loop.InertiaKgM2, Diagonal),
			static_cast<float>(0.5 * (RoverInertia.X + RoverInertia.Y)), 1e-2f);
		TestTrue(FString::Printf(TEXT("and its rate limit %.1f deg/s sits between the two axes"),
				FMath::RadiansToDegrees(Between)),
			Between >= FMath::Min(TwoGyros.X, TwoGyros.Y) - 1e-6f
			&& Between <= FMath::Max(TwoGyros.X, TwoGyros.Y) + 1e-6f);
	}
	return true;
}

/**
 * Reference handling, library coverage for the attitude hold the vehicle does
 * NOT currently engage: releasing the stick freezes the reference, and the
 * reference is leashed so it can never sit further from the hull than the
 * installed torque can hold.
 *
 * Every axis is exercised. The previous version tested pitch only, which is
 * exactly why a roll leash of 364 degrees - more than a full turn, so no leash
 * at all - shipped unnoticed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerAttitudeReferenceLeash, "Exoneer.Attitude.ReferenceLeash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerAttitudeReferenceLeash::RunTest(const FString& Parameters)
{
	const FLoopParams Loop = RoverLoop();
	const FVector MaxError = MaxHoldableErrorRad(Loop, RoverRatedTorqueNm);

	// No axis may have a leash so loose that it is not a leash. Half a radian
	// past a right angle is already further than any pilot reads as "holding
	// attitude"; a leash near or past pi is a windup path, not a limit.
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		TestTrue(FString::Printf(TEXT("axis %d leash %.1f deg is a real limit"),
				Axis, FMath::RadiansToDegrees(static_cast<float>(MaxError[Axis]))),
			MaxError[Axis] < 2.f);
	}

	// A hull that cannot move while the pilot holds full rate for 30 s: on
	// EVERY axis the reference must stop at the leash, not wind to infinity.
	const FQuat Stuck = FQuat::Identity;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		FVector Command = FVector::ZeroVector;
		Command[Axis] = 1.f;   // 1 rad/s
		FQuat Reference = Stuck;
		for (int32 Step = 0; Step < 1800; ++Step)
		{
			Reference = AdvanceReference(Reference, Stuck, Command, 1.f / 60.f, MaxError);
		}
		const FVector Error = AttitudeErrorBody(Stuck, Reference);
		TestTrue(FString::Printf(TEXT("axis %d reference is leashed to %.3f rad (got %.3f)"),
				Axis, static_cast<float>(MaxError[Axis]), Error.Size()),
			Error.Size() <= MaxError[Axis] + 1e-3f);
		TestTrue(FString::Printf(TEXT("axis %d reference actually moved"), Axis), Error.Size() > 0.05f);

		// Zero command: the reference does not move at all.
		const FQuat Held = Reference;
		for (int32 Step = 0; Step < 600; ++Step)
		{
			Reference = AdvanceReference(Reference, Stuck, FVector::ZeroVector, 1.f / 60.f, MaxError);
		}
		TestTrue(FString::Printf(TEXT("axis %d released stick freezes the reference"), Axis),
			Reference.Equals(Held, 1e-4f));
	}

	// The error vector is the shortest arc and matches a known rotation.
	const FQuat Yawed(FVector::UpVector, FMath::DegreesToRadians(30.f));
	const FVector YawError = AttitudeErrorBody(FQuat::Identity, Yawed);
	TestEqual(TEXT("30 deg yaw reads as 30 deg about Z"),
		static_cast<float>(YawError.Z), FMath::DegreesToRadians(30.f), 1e-4f);
	TestTrue(TEXT("no spurious roll or pitch"), FMath::Abs(YawError.X) + FMath::Abs(YawError.Y) < 1e-4f);
	TestTrue(TEXT("identical attitudes have zero error"),
		AttitudeErrorBody(Yawed, Yawed).IsNearlyZero());
	return true;
}

/**
 * THE CLOSED LOOP WITH ROTOR MOMENTUM INSIDE IT, and a standing external
 * moment - the case the old suite could not see, because RunRateAxis simulates
 * an ideal torque source with infinite momentum capacity. Eight passing tests
 * were fully consistent with the craft being unflyable.
 *
 * WHAT THIS TEST NO LONGER CLAIMS. It used to be parameterised on
 * AvailableTrimTorqueNm = 2992 N*m, described as the trim authority the six
 * lift thrusters have at the hover setting. Nothing produced that number. Trim
 * authority is symmetric - a unit may only be biased as far as it can be biased
 * back - so it is a function of the collective, and at the two settings the
 * shipped control scheme reached most often, valves shut while falling and
 * valves wide while climbing, it was exactly ZERO. A recovery proved against an
 * authority the vehicle cannot have is not a proof of anything.
 *
 * So this test names no authority. It SWEEPS it, and asserts the shape of the
 * loop, which is a property of the loop rather than of a craft: with no sink a
 * standing moment always saturates and then runs away; with a sink that can
 * reach the moment the axis holds indefinitely and the rotors stay near empty;
 * in between, every share of authority buys measurable time. The authority the
 * shipped rover really has at the collectives it really flies, and the momentum
 * it really spends over a two-minute sortie, are measured by running the whole
 * vehicle loop with the real allocator in
 * Exoneer.Thrust.RoverHoldsAuthorityForTwoMinutes.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerAttitudeMomentumInTheLoop, "Exoneer.Attitude.MomentumInTheLoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerAttitudeMomentumInTheLoop::RunTest(const FString& Parameters)
{
	const FLoopParams Loop = RoverLoop();
	constexpr int32 PitchAxis = 1;
	const float Kd = static_cast<float>(RateGain(Loop)[PitchAxis]);
	const float Inertia = static_cast<float>(RoverInertia[PitchAxis]);
	constexpr float Dt = 1.f / 60.f;

	struct FResult
	{
		float SaturatedAtSeconds = -1.f;
		float PeakRateRadS = 0.f;
		float FinalStoredNms = 0.f;
	};

	// One axis of the vehicle wiring: the rate law, the standing moment fed
	// forward into the triad's ask, per-axis saturation, rotor momentum
	// integration, and a thruster pair that supplies an external torque the
	// triad mirrors exactly. AvailableTrimTorqueNm is a SWEPT parameter here,
	// never a claim about a craft.
	auto Simulate = [&](float StandingNm, float AvailableTrimTorqueNm,
		float DumpRate, float DumpOnset, float Seconds) -> FResult
	{
		FResult Result;
		float Rate = 0.f;
		float Stored = 0.f;
		const int32 Steps = FMath::RoundToInt(Seconds / Dt);
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			const float Attitude = FMath::Clamp(Kd * (0.f - Rate), -RoverRatedTorqueNm, RoverRatedTorqueNm);

			// The trim cancels the standing moment first - that is its primary
			// job, and it costs no rotor momentum at all - then unwinds on top
			// of that once the store is past the authored onset.
			float ThrusterTorque = FMath::Clamp(-StandingNm, -AvailableTrimTorqueNm, AvailableTrimTorqueNm);
			const float RemainingTrim = FMath::Max(0.f, AvailableTrimTorqueNm - FMath::Abs(ThrusterTorque));
			if (DumpRate > 0.f && FMath::Abs(Stored) / RoverCapacityNms > DumpOnset)
			{
				// Headroom from the torque the triad will REALLY deliver, not
				// from the torque the rate law asked for. With the store at its
				// stop the ask is full rating, so the old arithmetic read zero
				// headroom exactly when the dump was needed most.
				const float Ask = FMath::Clamp(Attitude - StandingNm, -RoverRatedTorqueNm, RoverRatedTorqueNm);
				const float Deliverable = (FMath::Abs(Stored) >= RoverCapacityNms && Stored * -Ask > 0.f) ? 0.f : Ask;
				const float Headroom = FMath::Max(0.f, RoverRatedTorqueNm - FMath::Abs(Deliverable));
				const float Unwind = FMath::Clamp(Stored * DumpRate, -Headroom, Headroom);
				ThrusterTorque += FMath::Clamp(-Unwind, -RemainingTrim, RemainingTrim);
			}

			float Command = FMath::Clamp(Attitude - StandingNm - ThrusterTorque,
				-RoverRatedTorqueNm, RoverRatedTorqueNm);
			// A saturated rotor cannot wind further in the same direction.
			if (FMath::Abs(Stored) >= RoverCapacityNms && Stored * -Command > 0.f)
			{
				Command = 0.f;
			}
			if (FMath::Abs(Stored) >= RoverCapacityNms * 0.999f && Result.SaturatedAtSeconds < 0.f)
			{
				Result.SaturatedAtSeconds = Step * Dt;
			}
			Rate += ((Command + ThrusterTorque + StandingNm) / Inertia - Loop.HullAngularDamping * Rate) * Dt;
			Stored -= Command * Dt;
			Stored = FMath::Clamp(Stored, -RoverCapacityNms, RoverCapacityNms);
			Result.PeakRateRadS = FMath::Max(Result.PeakRateRadS, FMath::Abs(Rate));
		}
		Result.FinalStoredNms = Stored;
		return Result;
	};

	// The standing moment the shipped rover's forward pair made at full thrust
	// before the pair was moved to straddle the centre of mass. This is the
	// number that took the pitch axis out in flight.
	constexpr float PreviousStandingNm = 1223.f;

	// NO SINK AT ALL, which is what the vehicle had: the axis saturates and
	// then the moment is simply unopposed. The reference case.
	const FResult NoSink = Simulate(PreviousStandingNm, 0.f, 0.f, 1.f, 30.f);
	TestTrue(FString::Printf(TEXT("with no sink the axis saturates, after %.2f s"), NoSink.SaturatedAtSeconds),
		NoSink.SaturatedAtSeconds > 0.f && NoSink.SaturatedAtSeconds < 2.f);
	TestTrue(FString::Printf(TEXT("and then the moment is unopposed: %.2f rad/s"), NoSink.PeakRateRadS),
		NoSink.PeakRateRadS > 1.f);

	// A SINK THAT REACHES THE MOMENT. It never saturates over two minutes, the
	// rotors stay near empty because thrust is holding the moment instead of
	// them, and the hull does not move.
	const FResult Carried = Simulate(PreviousStandingNm, PreviousStandingNm * 1.2f,
		RoverDumpRatePerSec, RoverDumpOnsetFraction, 120.f);
	TestTrue(TEXT("a trim that reaches the standing moment never saturates, over two minutes"),
		Carried.SaturatedAtSeconds < 0.f);
	TestTrue(FString::Printf(TEXT("and the rotors stay near empty: %.0f N*m*s"), Carried.FinalStoredNms),
		FMath::Abs(Carried.FinalStoredNms) < RoverCapacityNms * 0.05f);
	TestTrue(FString::Printf(TEXT("and the hull barely moves: peak %.2f deg/s"),
			FMath::RadiansToDegrees(Carried.PeakRateRadS)),
		Carried.PeakRateRadS < FMath::DegreesToRadians(5.f));

	// THE SWEEP. Every share of authority buys strictly more time than the last,
	// so the relationship is monotone and no single hand-picked value is load
	// bearing. A reaction wheel still has a real limit; the visor is what tells
	// the pilot where it is.
	float PreviousSaturation = NoSink.SaturatedAtSeconds;
	for (const float Share : { 0.25f, 0.5f, 0.75f, 0.9f })
	{
		const FResult Partial = Simulate(PreviousStandingNm, PreviousStandingNm * Share,
			RoverDumpRatePerSec, RoverDumpOnsetFraction, 120.f);
		TestTrue(FString::Printf(TEXT("%.0f%% authority delays saturation to %.2f s, past %.2f s"),
				Share * 100.f, Partial.SaturatedAtSeconds, PreviousSaturation),
			Partial.SaturatedAtSeconds > PreviousSaturation);
		PreviousSaturation = Partial.SaturatedAtSeconds;
	}

	// THE GUARD THAT LET B7 THROUGH, corrected. It compared the dump onset
	// against I*w only - the cost of REACHING the rate limit - and concluded
	// that a full stick input sat comfortably inside the envelope. It does, for
	// an instant. Holding it costs D*I*w every second on top, so the honest
	// comparison is the spin-up PLUS the authored hold time, and that is what
	// the rate limit is now sized from.
	const FVector RateLimit = MaxCommandRateRadS(Loop, RoverCapacityNms,
		RoverCommandMomentumFraction, FMath::DegreesToRadians(RoverRateCeilingDegPerSec),
		RoverSustainedTurnSeconds);
	const float CostToReachRateLimit = Inertia * static_cast<float>(RateLimit[PitchAxis]);
	const float CostToHoldPerSecond = static_cast<float>(Loop.HullAngularDamping) * CostToReachRateLimit;
	const float OnsetNms = RoverCapacityNms * RoverDumpOnsetFraction;
	AddInfo(FString::Printf(
		TEXT("pitch at %.1f deg/s: %.0f N*m*s to reach, %.0f N*m*s per second to hold, ")
		TEXT("so %.0f s of held pitch reaches %.0f N*m*s against a %.0f N*m*s dump onset"),
		FMath::RadiansToDegrees(static_cast<float>(RateLimit[PitchAxis])), CostToReachRateLimit,
		CostToHoldPerSecond, RoverSustainedTurnSeconds,
		CostToReachRateLimit + CostToHoldPerSecond * RoverSustainedTurnSeconds, OnsetNms));
	TestTrue(FString::Printf(TEXT("a full pitch command costs %.0f N*m*s to reach, under the %.0f N*m*s onset"),
			CostToReachRateLimit, OnsetNms),
		CostToReachRateLimit < OnsetNms);
	TestTrue(FString::Printf(TEXT("and %.0f s of HOLDING it still stays under the onset (%.0f N*m*s)"),
			RoverSustainedTurnSeconds,
			CostToReachRateLimit + CostToHoldPerSecond * RoverSustainedTurnSeconds),
		CostToReachRateLimit + CostToHoldPerSecond * RoverSustainedTurnSeconds < OnsetNms);

	// AND THE HOLD COST IS ITSELF A STANDING MOMENT, which is the insight the
	// architecture now rests on: run the same simulation with the hold torque
	// as the standing moment and it behaves identically. With no thrust sink it
	// saturates; with one that reaches it, it never does.
	{
		const float HoldNm = static_cast<float>(HoldTorqueNm(Loop, FVector(0.f, RateLimit.Y, 0.f)).Y);
		const FResult NoYawSink = Simulate(HoldNm, 0.f, RoverDumpRatePerSec, RoverDumpOnsetFraction, 120.f);
		const FResult WithSink = Simulate(HoldNm, HoldNm * 2.f, RoverDumpRatePerSec, RoverDumpOnsetFraction, 120.f);
		AddInfo(FString::Printf(
			TEXT("holding the pitch rate limit is a %.0f N*m standing moment: with no thrust sink the axis ")
			TEXT("saturates after %.1f s, with one it never does and the rotors end at %.0f N*m*s"),
			HoldNm, NoYawSink.SaturatedAtSeconds, WithSink.FinalStoredNms));
		TestTrue(FString::Printf(TEXT("with no sink, holding a rate saturates the axis after %.1f s"),
				NoYawSink.SaturatedAtSeconds),
			NoYawSink.SaturatedAtSeconds > 0.f && NoYawSink.SaturatedAtSeconds < 60.f);
		TestTrue(TEXT("with a thrust sink that reaches it, the axis never saturates in two minutes"),
			WithSink.SaturatedAtSeconds < 0.f);
		TestTrue(FString::Printf(TEXT("and the rotors stay near empty: %.0f N*m*s"), WithSink.FinalStoredNms),
			FMath::Abs(WithSink.FinalStoredNms) < RoverCapacityNms * 0.05f);
	}
	return true;
}

/**
 * B8, the half that is a CONTROL defect rather than a governor defect: the
 * pilot needs a way back to level, and before this there was none.
 *
 * A thrust vehicle has no restoring moment about its own centre of mass. Bank
 * it 20 degrees and it accelerates sideways at 3.6 m/s^2 for ever, decaying
 * only into linear damping - a 69 m/s drift from a bump. The lift governor
 * makes it worse, not better, because a banked craft is asked for MORE valve.
 * Nothing in the vehicle could end that state.
 *
 * So the reference holds roll and pitch, releases yaw every frame, and slews
 * back to level when the stick is released. This runs the cascade the vehicle
 * runs - reference error to a bounded rate command to the single rate loop -
 * and checks all four properties: it tracks a held stick, it returns to level
 * on release, it never holds a heading, and it cannot wind up.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerAttitudeLevelReturn, "Exoneer.Attitude.LevelReturn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerAttitudeLevelReturn::RunTest(const FString& Parameters)
{
	const FLoopParams Loop = RoverLoop();
	const FVector RateLimit = MaxCommandRateRadS(Loop, RoverCapacityNms, RoverCommandMomentumFraction,
		FMath::DegreesToRadians(RoverRateCeilingDegPerSec), RoverSustainedTurnSeconds);
	const FVector MaxError = MaxTrackableErrorRad(Loop, RateLimit);
	const FVector Kd = RateGain(Loop);
	const float LevelRateRadS = FMath::DegreesToRadians(RoverLevelRateDegPerSec);
	constexpr float Dt = 1.f / 60.f;

	// The leash is derived, not authored: the reference never sits further from
	// the hull than the outer loop can ask for at full rate.
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		TestEqual(FString::Printf(TEXT("axis %d leash is RateLimit x SettleTime"), Axis),
			static_cast<float>(MaxError[Axis]),
			static_cast<float>(RateLimit[Axis]) * RoverSettleTimeSeconds, 1e-5f);
	}

	// ONE FRAME OF THE VEHICLE LOOP, roll/pitch held and yaw on rate.
	struct FState
	{
		FQuat Body = FQuat::Identity;
		FQuat Reference = FQuat::Identity;
		FVector Rate = FVector::ZeroVector;
		FVector PeakStored = FVector::ZeroVector;
		FVector Stored = FVector::ZeroVector;
	};
	auto Step = [&](FState& S, const FVector& StickRateLocal)
	{
		const FVector RollPitch(StickRateLocal.X, StickRateLocal.Y, 0.0);
		S.Reference = AdvanceReference(S.Reference, S.Body, RollPitch, Dt, MaxError);
		if (RollPitch.IsNearlyZero())
		{
			S.Reference = LevelReference(S.Reference, LevelRateRadS * Dt);
		}
		S.Reference = ReleaseBodyAxis(S.Reference, S.Body, 2);
		S.Reference = LeashReference(S.Reference, S.Body, MaxError);

		FVector Error = AttitudeErrorBody(S.Body, S.Reference);
		Error.Z = 0.0;
		const FVector Command(
			FMath::Clamp(Error.X / RoverSettleTimeSeconds, -RateLimit.X, RateLimit.X),
			FMath::Clamp(Error.Y / RoverSettleTimeSeconds, -RateLimit.Y, RateLimit.Y),
			StickRateLocal.Z);
		const FVector Torque = ((Command - S.Rate) * Kd).BoundToCube(RoverRatedTorqueNm);
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			S.Rate[Axis] += (Torque[Axis] / RoverInertia[Axis] - Loop.HullAngularDamping * S.Rate[Axis]) * Dt;
			S.Stored[Axis] = FMath::Clamp(S.Stored[Axis] - Torque[Axis] * Dt, -RoverCapacityNms, RoverCapacityNms);
			S.PeakStored[Axis] = FMath::Max(S.PeakStored[Axis], FMath::Abs(S.Stored[Axis]));
		}
		const FVector WorldRate = S.Body.RotateVector(S.Rate);
		const double Angle = WorldRate.Size() * Dt;
		if (Angle > UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			S.Body = (FQuat(WorldRate.GetSafeNormal(), Angle) * S.Body).GetNormalized();
		}
	};

	// (1) A HELD STICK TRACKS. Three seconds of full roll and the craft is
	//     banked by about the commanded rate times the time.
	FState Roll;
	for (int32 i = 0; i < FMath::RoundToInt(3.f / Dt); ++i)
	{
		Step(Roll, FVector(RateLimit.X, 0.0, 0.0));
	}
	const float BankDeg = static_cast<float>(Roll.Body.Rotator().Roll);
	const float ExpectedDeg = FMath::RadiansToDegrees(static_cast<float>(RateLimit.X)) * 3.f;
	TestTrue(FString::Printf(TEXT("3 s of full roll reaches %.0f degrees of bank, asking for %.0f"),
			BankDeg, ExpectedDeg),
		FMath::Abs(BankDeg) > ExpectedDeg * 0.8f);

	// (2) RELEASING RETURNS IT TO LEVEL, which is the property that did not
	//     exist. Timed, so "it returns" is a number.
	float LevelledAt = -1.f;
	for (int32 i = 0; i < FMath::RoundToInt(15.f / Dt); ++i)
	{
		Step(Roll, FVector::ZeroVector);
		if (LevelledAt < 0.f && FMath::Abs(Roll.Body.Rotator().Roll) < 1.f)
		{
			LevelledAt = i * Dt;
		}
	}
	AddInfo(FString::Printf(
		TEXT("3 s of held roll reached %.0f deg of bank; releasing returned it to level in %.1f s, ")
		TEXT("peak rotor store %.0f / %.0f / %.0f N*m*s"),
		BankDeg, LevelledAt, Roll.PeakStored.X, Roll.PeakStored.Y, Roll.PeakStored.Z));
	TestTrue(FString::Printf(TEXT("releasing the stick returns the craft to level in %.1f s"), LevelledAt),
		LevelledAt > 0.f && LevelledAt < 8.f);
	TestTrue(FString::Printf(TEXT("and it stays there: %.2f deg of bank, %.2f of pitch"),
			Roll.Body.Rotator().Roll, Roll.Body.Rotator().Pitch),
		FMath::Abs(Roll.Body.Rotator().Roll) < 1.f && FMath::Abs(Roll.Body.Rotator().Pitch) < 1.f);
	TestTrue(FString::Printf(TEXT("the whole manoeuvre costs %.0f%% of the roll envelope"),
			100.f * Roll.PeakStored.X / RoverCapacityNms),
		Roll.PeakStored.X < RoverCapacityNms * 0.6f);

	// (3) A BUMP DECAYS. The hull is knocked to 25 degrees with the stick
	//     never touched; before this it stayed there for ever.
	{
		FState Bumped;
		Bumped.Body = FQuat(FRotator(0.f, 0.f, 25.f));
		Bumped.Reference = Bumped.Body;
		for (int32 i = 0; i < FMath::RoundToInt(12.f / Dt); ++i)
		{
			Step(Bumped, FVector::ZeroVector);
		}
		const float Residual = static_cast<float>(Bumped.Body.Rotator().Roll);
		const float SidewaysMS2 = 9.8f * FMath::Tan(FMath::DegreesToRadians(25.f));
		AddInfo(FString::Printf(
			TEXT("a 25 degree bump is a %.1f m/s2 permanent sideways acceleration with no level return; ")
			TEXT("with one it decays to %.2f degrees in 12 s"), SidewaysMS2, Residual));
		TestTrue(FString::Printf(TEXT("a 25 degree bump decays to %.2f degrees"), Residual),
			FMath::Abs(Residual) < 1.f);
	}

	// (4) YAW IS NEVER HELD, and levelling does not walk the heading.
	{
		FState Yawing;
		for (int32 i = 0; i < FMath::RoundToInt(6.f / Dt); ++i)
		{
			Step(Yawing, FVector(0.0, 0.0, RateLimit.Z));
		}
		const float Heading = static_cast<float>(Yawing.Body.Rotator().Yaw);
		TestTrue(FString::Printf(TEXT("yaw is a rate command: 6 s of stick turned %.0f degrees"), Heading),
			FMath::Abs(Heading) > 30.f);
		// Release: the heading must STAY where it was left, not snap back.
		const float Before = Heading;
		for (int32 i = 0; i < FMath::RoundToInt(6.f / Dt); ++i)
		{
			Step(Yawing, FVector::ZeroVector);
		}
		const float After = static_cast<float>(Yawing.Body.Rotator().Yaw);
		TestTrue(FString::Printf(TEXT("and the heading is not held: %.0f deg becomes %.0f, it does not return"),
				Before, After),
			FMath::Abs(FRotator::NormalizeAxis(After - Before)) < 25.f);
		TestTrue(FString::Printf(TEXT("while roll and pitch still come back level (%.2f, %.2f)"),
				Yawing.Body.Rotator().Roll, Yawing.Body.Rotator().Pitch),
			FMath::Abs(Yawing.Body.Rotator().Roll) < 2.f
			&& FMath::Abs(Yawing.Body.Rotator().Pitch) < 2.f);
		// ReleaseBodyAxis on its own: the reference's yaw error is gone.
		const FQuat Reference = FQuat(FRotator(5.f, 40.f, 5.f));
		const FQuat Released = ReleaseBodyAxis(Reference, FQuat::Identity, 2);
		TestTrue(FString::Printf(TEXT("ReleaseBodyAxis zeroes the yaw error (%.4f rad)"),
				AttitudeErrorBody(FQuat::Identity, Released).Z),
			FMath::Abs(AttitudeErrorBody(FQuat::Identity, Released).Z) < 1e-4);
	}

	// (5) IT CANNOT WIND UP. Hold the stick against a hull pinned by something
	//     else - which is what the suspension does on the ground - and the
	//     reference must stay inside the leash rather than storing an error to
	//     release the instant the wheels leave the ground.
	{
		FQuat Body = FQuat::Identity;
		FQuat Reference = FQuat::Identity;
		for (int32 i = 0; i < FMath::RoundToInt(30.f / Dt); ++i)
		{
			Reference = AdvanceReference(Reference, Body, FVector(RateLimit.X, 0.0, 0.0), Dt, MaxError);
			Reference = ReleaseBodyAxis(Reference, Body, 2);
			Reference = LeashReference(Reference, Body, MaxError);
		}
		const FVector Error = AttitudeErrorBody(Body, Reference);
		TestTrue(FString::Printf(TEXT("30 s of stick against a pinned hull stores %.1f deg, leashed at %.1f"),
				FMath::RadiansToDegrees(static_cast<float>(Error.X)),
				FMath::RadiansToDegrees(static_cast<float>(MaxError.X))),
			FMath::Abs(Error.X) <= MaxError.X + 1e-4);
	}

	// LevelReference itself: it moves roll and pitch toward zero and leaves the
	// heading alone, and an exactly inverted reference is left to the pilot
	// rather than resolved arbitrarily.
	{
		const FQuat Tilted(FRotator(20.f, 70.f, -30.f));
		const FQuat Levelled = LevelReference(Tilted, FMath::DegreesToRadians(90.f));
		TestTrue(FString::Printf(TEXT("LevelReference levels roll and pitch (%.2f, %.2f)"),
				Levelled.Rotator().Roll, Levelled.Rotator().Pitch),
			FMath::Abs(Levelled.Rotator().Roll) < 0.5f && FMath::Abs(Levelled.Rotator().Pitch) < 0.5f);
		TestEqual(TEXT("and leaves the heading where it was"),
			static_cast<float>(Levelled.Rotator().Yaw), 70.f, 0.5f);
		// Inverted comes back the short way round, which is what the pilot
		// wants; a reference with the nose straight up has no heading to keep
		// and is left to him instead of resolved arbitrarily.
		const FQuat Inverted(FRotator(0.f, 40.f, 180.f));
		const FQuat Recovered = LevelReference(Inverted, FMath::DegreesToRadians(180.f));
		TestTrue(FString::Printf(TEXT("an inverted reference is recovered (%.2f deg of roll left)"),
				Recovered.Rotator().Roll),
			FMath::Abs(Recovered.Rotator().Roll) < 0.5f);
		TestEqual(TEXT("still without turning it"), static_cast<float>(Recovered.Rotator().Yaw), 40.f, 0.5f);
		const FQuat NoseUp(FRotator(90.f, 0.f, 0.f));
		TestTrue(TEXT("a reference with no heading to keep is left to the pilot"),
			LevelReference(NoseUp, FMath::DegreesToRadians(10.f)).Equals(NoseUp, 1e-4f));
	}
	return true;
}

/**
 * THE CROSS TERM, INTEGRATED. Exoneer.Attitude.CrossTermIsPowerNeutral proves
 * the instantaneous dot(-w x h, w) is zero; that is necessary and not
 * sufficient. Applied by explicit Euler, a torque that is only INSTANTANEOUSLY
 * perpendicular to w adds rotational energy at about 0.5*dt*|tau|^2/I per
 * step - roughly 1.3 W at h = 1600 N*m*s, w = 0.4 rad/s, dt = 1/120.
 *
 * With a pilot aboard the rate null swamps it. WITH THE SEAT EMPTY the
 * commanded torque is exactly zero, the cross term is the only torque on the
 * hull, and momentum never bleeds airborne - so an abandoned craft with wound
 * rotors precessed with a slowly GROWING rate all the way to the ground. That
 * is the owner's third report ("leaving the vehicle mid air") in a new form.
 *
 * UGyroModule evaluates the term at the end-of-step rate instead, one Picard
 * step toward the implicit solution, which is dissipative rather than
 * generative. This test integrates both ways and compares.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerAttitudeCrossTermDoesNotAddEnergy, "Exoneer.Attitude.CrossTermDoesNotAddEnergy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerAttitudeCrossTermDoesNotAddEnergy::RunTest(const FString& Parameters)
{
	// The abandoned-craft case exactly: rotors at their stop on one axis, no
	// pilot, no command, no ground contact, 60 seconds of fall.
	const FVector Stored(0.f, 0.f, RoverCapacityNms);
	constexpr float Dt = 1.f / 120.f;
	constexpr float Seconds = 60.f;

	auto Energy = [](const FVector& Rate)
	{
		return 0.5 * (Rate.X * Rate.X * RoverInertia.X
			+ Rate.Y * Rate.Y * RoverInertia.Y
			+ Rate.Z * Rate.Z * RoverInertia.Z);
	};
	// ENERGY IS THE MEASURE, not the rate magnitude. The cross term moves
	// momentum BETWEEN axes, and on an asymmetric hull |w| rises whenever
	// momentum lands on a lighter axis even though the energy has fallen -
	// which is exactly the free precession a tumbling body does. Only the
	// energy can distinguish a numerical gain from real physics.
	auto Run = [&](bool bEndOfStep, double& OutStartEnergy, double& OutEndEnergy, double& OutPeakEnergy)
	{
		FVector Rate(0.35f, -0.2f, 0.05f);   // a hull tumbling as it was left
		OutStartEnergy = Energy(Rate);
		OutPeakEnergy = OutStartEnergy;
		const int32 Steps = FMath::RoundToInt(Seconds / Dt);
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			FVector RateForCross = Rate;
			if (bEndOfStep)
			{
				const FVector First = GyroscopicTorque(Rate, Stored);
				RateForCross = Rate + FVector(First.X / RoverInertia.X, First.Y / RoverInertia.Y,
					First.Z / RoverInertia.Z) * Dt;
			}
			const FVector Torque = GyroscopicTorque(RateForCross, Stored);
			Rate += FVector(Torque.X / RoverInertia.X, Torque.Y / RoverInertia.Y,
				Torque.Z / RoverInertia.Z) * Dt;
			OutPeakEnergy = FMath::Max(OutPeakEnergy, Energy(Rate));
		}
		OutEndEnergy = Energy(Rate);
	};

	double ExplicitStart = 0.0, ExplicitEnd = 0.0, ImplicitStart = 0.0, ImplicitEnd = 0.0;
	double ExplicitPeak = 0.0, ImplicitPeak = 0.0;
	Run(false, ExplicitStart, ExplicitEnd, ExplicitPeak);
	Run(true, ImplicitStart, ImplicitEnd, ImplicitPeak);

	AddInfo(FString::Printf(
		TEXT("an abandoned craft with saturated rotors, %.0f s at %.0f fps: explicit Euler takes the ")
		TEXT("rotational energy from %.1f J to %.1f J (%+.1f%%, peak %.1f J); evaluated at the ")
		TEXT("end-of-step rate it goes to %.1f J (%+.1f%%, peak %.1f J)"),
		Seconds, 1.f / Dt, ExplicitStart, ExplicitEnd,
		100.0 * (ExplicitEnd - ExplicitStart) / ExplicitStart, ExplicitPeak,
		ImplicitEnd, 100.0 * (ImplicitEnd - ImplicitStart) / ImplicitStart, ImplicitPeak));

	// THE DEFECT, measured: explicit Euler grows the energy.
	TestTrue(FString::Printf(TEXT("explicit Euler adds energy: %+.2f%% over %.0f s"),
			100.0 * (ExplicitEnd - ExplicitStart) / ExplicitStart, Seconds),
		ExplicitEnd > ExplicitStart * 1.001);

	// THE FIX: the same term evaluated one step ahead never adds any, at any
	// point in the run - so an abandoned craft precesses and slowly settles
	// instead of slowly winding up.
	TestTrue(FString::Printf(TEXT("the end-of-step form does not: %+.4f%%"),
			100.0 * (ImplicitEnd - ImplicitStart) / ImplicitStart),
		ImplicitEnd <= ImplicitStart * 1.000001);
	TestTrue(FString::Printf(TEXT("and never does at any point in the run (peak %.3f of %.3f J)"),
			ImplicitPeak, ImplicitStart),
		ImplicitPeak <= ImplicitStart * 1.000001);

	// Still power-neutral instantaneously, which is the property the term is
	// supposed to have and the reason it may be integrated either way at all.
	{
		FRandomStream Random(20260904);
		for (int32 Trial = 0; Trial < 200; ++Trial)
		{
			const FVector Rate = Random.VRand() * Random.FRandRange(0.01f, 3.f);
			const FVector Momentum = Random.VRand() * Random.FRandRange(1.f, RoverCapacityNms);
			const FVector Torque = GyroscopicTorque(Rate, Momentum);
			const double Power = FVector::DotProduct(Torque, Rate);
			TestTrue(FString::Printf(TEXT("trial %d transports no power (%.3e W)"), Trial, Power),
				FMath::Abs(Power) <= 1e-6 * FMath::Max(1.0, Torque.Size() * Rate.Size()));
		}
	}
	return true;
}

/**
 * The authored constants that must not be set independently of each other, and
 * the ones the numbers in this file are pinned to. Checked against the block
 * definition's own defaults, so the C++ and the bootstrap cannot drift apart
 * silently either.
 */
/**
 * B13, THE HALF THAT IS AN ATTITUDE PROBLEM. A/D is FULL-DEFLECTION roll -
 * PlayerSurvivalCharacter::Input_Move writes Rotate.Z straight from the axis -
 * and the reference had no ABSOLUTE bound at all. LeashReference bounds it
 * against the HULL, which is anti-windup and says nothing about where the hull
 * ends up, so one held key rolled the reference through 56 degrees in 3 s, 95.6
 * degrees in 5 s and past inverted in 10. Past 90 degrees the lift valve was
 * pointing at the ground.
 *
 * LimitReferenceTilt is a flight-envelope limiter and nothing more: it refuses
 * to COMMAND an attitude the craft cannot hold its weight at, applies no torque
 * of its own, and does not stop a collision putting the hull outside the cone -
 * the reference re-seeds from the hull and the pilot flies it back.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerAttitudeBankCeilingBoundsTheReference,
	"Exoneer.Attitude.BankCeilingBoundsTheReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerAttitudeBankCeilingBoundsTheReference::RunTest(const FString& Parameters)
{
	const FLoopParams Loop = RoverLoop();
	const FVector RateLimit = MaxCommandRateRadS(Loop, RoverCapacityNms, RoverCommandMomentumFraction,
		FMath::DegreesToRadians(RoverRateCeilingDegPerSec), RoverSustainedTurnSeconds);
	const FVector MaxError = MaxTrackableErrorRad(Loop, RateLimit);
	const FVector Kd = RateGain(Loop);
	const float CeilingRad = FMath::DegreesToRadians(RoverBankCeilingDeg);
	constexpr float Dt = 1.f / 60.f;

	// THE LIMITER ITSELF, on its own. Inside the cone it is the identity; outside
	// it lands exactly ON the cone, and it keeps the heading while doing so.
	{
		for (const float Deg : { 0.f, 10.f, 29.9f })
		{
			const FQuat Inside(FRotator(0.f, 37.f, Deg));
			TestTrue(FString::Printf(TEXT("%.1f degrees of bank is inside the cone and untouched"), Deg),
				LimitReferenceTilt(Inside, CeilingRad).Equals(Inside, 1e-5f));
		}
		for (const float Deg : { 45.f, 90.f, 140.f, 179.f })
		{
			const FQuat Outside(FRotator(0.f, 37.f, Deg));
			const FQuat Limited = LimitReferenceTilt(Outside, CeilingRad);
			TestEqual(FString::Printf(TEXT("%.0f degrees of bank is pulled back to the %.0f degree cone"),
					Deg, RoverBankCeilingDeg),
				FMath::RadiansToDegrees(ReferenceTiltRad(Limited)), RoverBankCeilingDeg, 0.05f);
			// Heading kept: a limiter that walks the azimuth is an autopilot.
			const FVector Flat(Limited.GetAxisX().X, Limited.GetAxisX().Y, 0.0);
			const FVector WasFlat(Outside.GetAxisX().X, Outside.GetAxisX().Y, 0.0);
			if (!Flat.IsNearlyZero() && !WasFlat.IsNearlyZero())
			{
				TestEqual(FString::Printf(TEXT("and the heading is unchanged at %.0f degrees of bank"), Deg),
					static_cast<float>(FMath::RadiansToDegrees(
						FMath::Atan2(Flat.GetSafeNormal().Y, Flat.GetSafeNormal().X))),
					static_cast<float>(FMath::RadiansToDegrees(
						FMath::Atan2(WasFlat.GetSafeNormal().Y, WasFlat.GetSafeNormal().X))), 0.5f);
			}
		}
		// A pitch-and-roll combination is limited on the TOTAL tilt, not per
		// axis: 30 degrees of each is 41.4 degrees of lift-axis tilt, which is
		// past the angle the craft can hold weight at.
		const FQuat Combined(FRotator(30.f, 0.f, 30.f));
		TestTrue(FString::Printf(TEXT("30 degrees of roll AND pitch is %.1f degrees of tilt"),
				FMath::RadiansToDegrees(ReferenceTiltRad(Combined))),
			FMath::RadiansToDegrees(ReferenceTiltRad(Combined)) > RoverHoldWeightLimitDeg);
		// Half a degree of tolerance, not a twentieth: the level slew's arc is
		// exactly along the tilt direction for a pure roll and a little off it
		// for a combined attitude, so a combined case lands 0.2 degrees outside
		// the cone and is pulled the rest of the way on the next frame.
		TestEqual(TEXT("and it is limited on the total, not per axis"),
			FMath::RadiansToDegrees(ReferenceTiltRad(LimitReferenceTilt(Combined, CeilingRad))),
			RoverBankCeilingDeg, 0.5f);
		// Zero disables it, so a craft with no authored ceiling is unchanged.
		TestTrue(TEXT("a zero ceiling disables the limiter"),
			LimitReferenceTilt(Combined, 0.f).Equals(Combined, 1e-6f));
	}

	// THE LOOP, with the stick pinned at full deflection for twenty seconds -
	// which is what "hold D" is. Both variants, so the fix is a measurement.
	struct FRun
	{
		float PeakTiltDeg = 0.f;
		float TiltAt3s = 0.f;
		float TiltAt5s = 0.f;
		float TiltAt10s = 0.f;
	};
	FRun Runs[2];
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const bool bLimited = (Pass == 1);
		FQuat Body = FQuat::Identity;
		FQuat Reference = FQuat::Identity;
		FVector Rate = FVector::ZeroVector;
		const FVector Stick(RateLimit.X, 0.0, 0.0);   // full-deflection roll
		for (int32 Step = 0; Step < FMath::RoundToInt(20.f / Dt); ++Step)
		{
			Reference = AdvanceReference(Reference, Body, FVector(Stick.X, 0.0, 0.0), Dt, MaxError);
			Reference = ReleaseBodyAxis(Reference, Body, 2);
			if (bLimited)
			{
				Reference = LimitReferenceTilt(Reference, CeilingRad);
			}
			Reference = LeashReference(Reference, Body, MaxError);

			FVector Error = AttitudeErrorBody(Body, Reference);
			Error.Z = 0.0;
			const FVector Command(
				FMath::Clamp(Error.X / RoverSettleTimeSeconds, -RateLimit.X, RateLimit.X),
				FMath::Clamp(Error.Y / RoverSettleTimeSeconds, -RateLimit.Y, RateLimit.Y),
				0.0);
			const FVector Torque = ((Command - Rate) * Kd).BoundToCube(RoverRatedTorqueNm);
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				Rate[Axis] += (Torque[Axis] / RoverInertia[Axis] - Loop.HullAngularDamping * Rate[Axis]) * Dt;
			}
			if (!Rate.IsNearlyZero())
			{
				Body = (FQuat(Rate.GetSafeNormal(), Rate.Size() * Dt) * Body).GetNormalized();
			}
			const float TiltDeg = FMath::RadiansToDegrees(
				FMath::Acos(FMath::Clamp(Body.GetAxisZ().Z, -1.0, 1.0)));
			Runs[Pass].PeakTiltDeg = FMath::Max(Runs[Pass].PeakTiltDeg, TiltDeg);
			const float Seconds = (Step + 1) * Dt;
			if (FMath::IsNearlyEqual(Seconds, 3.f, Dt * 0.5f))  { Runs[Pass].TiltAt3s = TiltDeg; }
			if (FMath::IsNearlyEqual(Seconds, 5.f, Dt * 0.5f))  { Runs[Pass].TiltAt5s = TiltDeg; }
			if (FMath::IsNearlyEqual(Seconds, 10.f, Dt * 0.5f)) { Runs[Pass].TiltAt10s = TiltDeg; }
		}
	}

	// THE DEFECT: unbounded, one held key rolls the hull past 90 degrees, where
	// the lift valve points at the ground.
	TestTrue(FString::Printf(
			TEXT("unbounded, held roll reaches %.0f degrees at 3 s, %.0f at 5 s and %.0f at 10 s"),
			Runs[0].TiltAt3s, Runs[0].TiltAt5s, Runs[0].TiltAt10s),
		Runs[0].TiltAt3s > 40.f && Runs[0].TiltAt5s > 90.f && Runs[0].PeakTiltDeg > 120.f);

	// THE FIX: bounded, and bounded below the angle the craft can hold weight
	// at, so the lift governor can never be pinned by the stick alone.
	TestTrue(FString::Printf(TEXT("bounded, the same input settles at %.1f degrees and peaks at %.1f"),
			Runs[1].TiltAt10s, Runs[1].PeakTiltDeg),
		Runs[1].PeakTiltDeg < RoverBankCeilingDeg + 1.5f);
	TestTrue(FString::Printf(TEXT("which is inside the %.1f degree hold-weight limit"), RoverHoldWeightLimitDeg),
		Runs[1].PeakTiltDeg < RoverHoldWeightLimitDeg);
	// And it is not a lock: the pilot still gets the bank he needs to translate.
	TestTrue(FString::Printf(TEXT("and still buys %.1f m/s2 of lateral acceleration"),
			9.8f * FMath::Tan(FMath::DegreesToRadians(Runs[1].PeakTiltDeg))),
		Runs[1].PeakTiltDeg > RoverBankCeilingDeg - 2.f);

	// A COLLISION CAN STILL PUT THE HULL ANYWHERE, and there the limiter must
	// HELP the recovery rather than lock the craft out. Seeded from a hull
	// tumbled to 150 degrees, the reference is pulled to the cone on the first
	// frame - so the loop asks for a return to 30 degrees of bank, not for the
	// 150 degrees the hull is at - and the ask is still a bounded RATE, because
	// the leash runs after the limiter.
	{
		const FQuat Body(FRotator(0.f, 0.f, 150.f));
		FQuat Reference = Body;
		Reference = LevelReference(Reference, FMath::DegreesToRadians(RoverLevelRateDegPerSec) * Dt);
		Reference = ReleaseBodyAxis(Reference, Body, 2);
		Reference = LimitReferenceTilt(Reference, CeilingRad);
		TestEqual(FString::Printf(TEXT("a hull tumbled to 150 degrees gets a reference at %.1f degrees"),
				FMath::RadiansToDegrees(ReferenceTiltRad(Reference))),
			FMath::RadiansToDegrees(ReferenceTiltRad(Reference)), RoverBankCeilingDeg, 0.5f);
		Reference = LeashReference(Reference, Body, MaxError);
		const FVector Error = AttitudeErrorBody(Body, Reference);
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			TestTrue(FString::Printf(TEXT("and the ask on axis %d stays inside the leash (%.4f of %.4f rad)"),
					Axis, FMath::Abs(static_cast<float>(Error[Axis])), static_cast<float>(MaxError[Axis])),
				FMath::Abs(Error[Axis]) <= MaxError[Axis] + 1e-4);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerAttitudeConstantInvariants, "Exoneer.Attitude.ConstantInvariants",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerAttitudeConstantInvariants::RunTest(const FString& Parameters)
{
	const UVehicleBlockDefinitionDataAsset* Defaults = GetDefault<UVehicleBlockDefinitionDataAsset>();
	if (!Defaults)
	{
		AddError(TEXT("no block definition defaults"));
		return false;
	}

	// The dump onset has to sit ABOVE the share of the envelope a single
	// full-deflection stick command spends, or the desaturation path runs
	// during essentially every manoeuvre - lift thrusters in constant motion as
	// the normal state rather than as a near-saturation recovery. They were
	// authored 0.2 against 0.5, and nothing in the code or the tests noticed.
	TestTrue(FString::Printf(TEXT("dump onset %.2f sits above the command share %.2f"),
			Defaults->MomentumDumpOnsetFraction, Defaults->AttitudeCommandMomentumFraction),
		Defaults->MomentumDumpOnsetFraction > Defaults->AttitudeCommandMomentumFraction);

	// The tests above pin the authored values; if a retune moves the block
	// defaults, the numbers in this file have to move with them.
	TestEqual(TEXT("settle time matches the value the tests pin"),
		Defaults->AttitudeSettleTimeSeconds, RoverSettleTimeSeconds, 1e-4f);
	TestEqual(TEXT("rate ceiling matches the value the tests pin"),
		Defaults->AttitudeCommandRateCeilingDegPerSec, RoverRateCeilingDegPerSec, 1e-4f);
	TestEqual(TEXT("dump onset matches the value the tests pin"),
		Defaults->MomentumDumpOnsetFraction, RoverDumpOnsetFraction, 1e-4f);
	TestEqual(TEXT("dump rate matches the value the tests pin"),
		Defaults->MomentumDumpRatePerSec, RoverDumpRatePerSec, 1e-4f);
	TestEqual(TEXT("the sustained-turn budget matches the value the tests pin"),
		Defaults->AttitudeSustainedTurnSeconds, RoverSustainedTurnSeconds, 1e-4f);

	// THE DUMP RELEASE MUST SIT BELOW THE ONSET, or the hysteresis never
	// latches off and the dump runs for ever; and it must be above zero, or a
	// dump runs the store all the way to empty and spends thrust doing it.
	TestTrue(FString::Printf(TEXT("dump release %.2f sits below the onset %.2f"),
			Defaults->MomentumDumpReleaseFraction, Defaults->MomentumDumpOnsetFraction),
		Defaults->MomentumDumpReleaseFraction < Defaults->MomentumDumpOnsetFraction
		&& Defaults->MomentumDumpReleaseFraction > 0.f);

	// THE OFFLOAD LAG MUST BE SLOWER THAN THE ATTITUDE LOOP ITSELF, or the
	// differential-thrust path starts chasing transients - which is the one
	// thing it must never do, because a valve that moves for a transient moves
	// the altitude with it.
	TestTrue(FString::Printf(TEXT("the offload lag %.2f s is slower than the %.2f s attitude loop"),
			Defaults->AttitudeOffloadTimeConstantSeconds, Defaults->AttitudeSettleTimeSeconds),
		Defaults->AttitudeOffloadTimeConstantSeconds > Defaults->AttitudeSettleTimeSeconds * 4.f);

	// A WAY BACK TO LEVEL MUST EXIST. Without it a bump leaves a permanent
	// sideways acceleration, because a thrust vehicle has no restoring moment
	// about its own centre of mass.
	TestTrue(FString::Printf(TEXT("the reference returns to level at %.0f deg/s"),
			Defaults->AttitudeLevelRateDegPerSec),
		Defaults->AttitudeLevelRateDegPerSec > 0.f);
	TestTrue(TEXT("and never faster than the stick can command, or releasing it snaps"),
		Defaults->AttitudeLevelRateDegPerSec <= Defaults->AttitudeCommandRateCeilingDegPerSec + 1e-4f);

	// A BANK CEILING MUST EXIST, or full-deflection roll integrates the
	// reference straight through 90 degrees of bank and the lift valve ends up
	// pointing at the ground. It must sit at or below the angle the craft can
	// hold its own weight at, and it must be large enough to translate with.
	TestEqual(TEXT("the bank ceiling matches the value the tests pin"),
		Defaults->AttitudeBankCeilingDeg, RoverBankCeilingDeg, 1e-4f);
	TestTrue(FString::Printf(TEXT("the bank ceiling %.0f degrees is inside the %.1f degree hold-weight limit"),
			Defaults->AttitudeBankCeilingDeg, RoverHoldWeightLimitDeg),
		Defaults->AttitudeBankCeilingDeg > 0.f
		&& Defaults->AttitudeBankCeilingDeg <= RoverHoldWeightLimitDeg);
	TestTrue(FString::Printf(TEXT("and buys %.1f m/s2 of lateral acceleration"),
			9.8f * FMath::Tan(FMath::DegreesToRadians(Defaults->AttitudeBankCeilingDeg))),
		9.8f * FMath::Tan(FMath::DegreesToRadians(Defaults->AttitudeBankCeilingDeg)) > 3.f);

	// A NOZZLE CANT MUST EXIST ON THE THRUSTER, or yaw has no in-air effector
	// at all: thrusters pointing along the hull's up axis make exactly zero
	// yaw moment however they are throttled, and holding a yaw rate then costs
	// rotor momentum with no way to earn it back. It must stay small, because
	// the craft pays cos^2 of it in vertical lift.
	TestTrue(FString::Printf(TEXT("the lift nozzles are canted %.1f degrees"), Defaults->NozzleCantDeg),
		Defaults->NozzleCantDeg > 0.f && Defaults->NozzleCantDeg < 15.f);

	// THE DESCEND KEY MUST BE A BOUNDED RATE, not a valve kill: landing damage
	// starts at 8 m/s and the governor's arrest authority is a fifth of a g.
	TestTrue(FString::Printf(TEXT("the descend key governs to %.1f m/s"), Defaults->LiftDescentRateMS),
		Defaults->LiftDescentRateMS > 0.f && Defaults->LiftDescentRateMS < 8.f);

	// The contact bleed is the only ALWAYS-available momentum sink, so it must
	// exist. Without it a saturated triad on a wheel-less craft, or on a tipped
	// rover, can never recover - saturation stops being a setback and becomes a
	// dead vehicle. The gate on it is external SUPPORT rather than tyre
	// compression (AVehicleConstruct::IsSupportedByGround), because a hull
	// resting on the ground reacts the same torque a tyre does and a craft with
	// no wheel blocks has an empty wheel array.
	TestTrue(TEXT("a ground-contact momentum sink exists"), Defaults->MomentumGroundBleedPerSec > 0.f);

	// The ground/air decision is debounced on BOTH edges. It gates the lift
	// governor and nothing else: hover hold must not open the valve on a craft
	// sitting on its wheels, and one wheel tap must not slam it shut in flight.
	// The attitude loop is deliberately not gated on contact at all.
	TestTrue(TEXT("the contact decision is debounced"), Defaults->AttitudeGroundReleaseSeconds > 0.f);

	// A CONTROL RESERVE MUST EXIST, or the whole differential-thrust path is
	// unreachable at the top of the lever: the trim bound is symmetric, so a
	// valve the pilot can drive to 1.00 has zero authority to hold a standing
	// moment or unwind a rotor, and full collective is where a climbing pilot
	// sits. It also has to be small enough to leave the craft able to climb.
	TestTrue(FString::Printf(TEXT("a control reserve exists: %.2f"), Defaults->LiftControlReserveFraction),
		Defaults->LiftControlReserveFraction > 0.f);
	TestTrue(FString::Printf(TEXT("and it is not so large the craft cannot climb: ceiling %.2f"),
			1.f - Defaults->LiftControlReserveFraction),
		Defaults->LiftControlReserveFraction < 0.3f);

	// The lift governor needs a rate gain, or releasing both keys leaves the
	// craft drifting off the feed-forward's own error instead of holding.
	TestTrue(FString::Printf(TEXT("the lift governor has a rate gain: %.2f per m/s"),
			Defaults->LiftHoverDampingPerMS),
		Defaults->LiftHoverDampingPerMS > 0.f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
