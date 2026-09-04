// Copyright Exoneer contributors.
//
// Unit tests for ExoneerWheelSim::StepWheel - the per-substep wheel solve. The
// pure terramechanics library has its own suite; this one pins the behaviours
// the playtest reported as broken, so a future change cannot quietly bring them
// back:
//
//   * a parked wheel makes no tangential force, and a drifting one is HELD by
//     static friction instead of creeping through zero (report 1);
//   * losing and regaining contact does not step the normal load (report 5);
//   * the drawn wheel rests ON the ground rather than inside it (report 6),
//     and the rut the solver rides in is not folded into the drawn hub;
//   * firm ground develops its full interface friction and launches without
//     wheelspin, while clay still spins (report 4).
//
// Headless run:
//   UnrealEditor-Cmd.exe <uproject> -ExecCmds="Automation RunTests Exoneer.Vehicles.Wheel; Quit"
//     -TestExit="Automation Test Queue Empty" -unattended -nullrhi -nosplash -nop4 -log

#include "Misc/AutomationTest.h"
#include "Vehicles/WheelSimTypes.h"
#include "Vehicles/ExoneerVehicleUnits.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	using namespace ExoneerWheelSim;
	using namespace ExoneerTerramechanics;

	constexpr float SubstepDt = 1.f / 120.f;
	/** Test rover as the spawner builds it: ~1850 kg on six road wheels. */
	constexpr float RoverMassKg = 1850.f;
	constexpr int32 RoverWheelCount = 6;
	constexpr float CornerMassKg = RoverMassKg / (float)RoverWheelCount;
	/** Mount height that puts the road wheel near its static ride on flat ground. */
	constexpr float PlaneDepthUU = 56.7f;

	FWheelSimConfig RoadWheelConfig()
	{
		FWheelSimConfig Config;
		Config.RadiusM = 0.35f;
		Config.WidthM = 0.18f;
		Config.SpringNPerM = 30000.f;
		Config.DamperNSecPerM = 2100.f;
		Config.RestLengthM = 0.32f;
		Config.TravelM = 0.22f;
		Config.BumpStopNPerM = 400000.f;
		Config.BumpStopTravelM = 0.05f;
		Config.WheelInertiaKgM2 = 0.6f * 60.f * 0.35f * 0.35f;   // 0.6 m r^2, 60 kg block
		Config.StallTorqueNm = 300.f;
		Config.NoLoadSpeedRadS = 52.f;
		Config.MaxBrakeTorqueNm = 600.f;
		Config.RollingResistRigid = 0.008f;
		Config.RollingResistFlexible = 0.015f;
		Config.BearingDragNm = 1.5f;
		Config.TreadMobilisation = 0.70f;
		Config.HardSurfaceGrip = 1.f;
		Config.TreadShearModulusM = 0.0025f;
		Config.TreadShearModulusLateralM = 0.003f;
		Config.StickSpeedMS = 0.05f;
		return Config;
	}

	FSoilParams DrySandSoil()
	{
		FSoilParams Soil;
		Soil.Kc = 990.f; Soil.Kphi = 1528430.f; Soil.N0 = 1.1f; Soil.N1 = 0.9f;
		Soil.Cohesion = 1040.f; Soil.FrictionAngleRad = FMath::DegreesToRadians(28.f);
		Soil.ShearK = 0.025f; Soil.ShearKy = 0.030f; Soil.UnitWeight = 15700.f;
		return Soil;
	}

	FSoilParams ClaySoil()
	{
		FSoilParams Soil;
		Soil.Kc = 13190.f; Soil.Kphi = 692150.f; Soil.N0 = 0.5f; Soil.N1 = 0.4f;
		Soil.Cohesion = 4140.f; Soil.FrictionAngleRad = FMath::DegreesToRadians(13.f);
		Soil.ShearK = 0.010f; Soil.ShearKy = 0.012f; Soil.UnitWeight = 16800.f;
		return Soil;
	}

	FWheelSimInputItem MakeInput(const FSoilParams& Soil)
	{
		FWheelSimInputItem Item;
		Item.BlockInstanceId = 1;
		Item.Config = RoadWheelConfig();
		Item.Command.Throttle = 0.f;
		Item.Command.Brake = 0.f;
		Item.Command.bParkingBrake = true;
		Item.Command.SlipCap = 1.f;
		Item.Command.TirePressurePa = 220000.f;
		Item.Command.CarcassPressurePa = 15000.f;
		Item.Command.SupplyFraction = 1.f;

		Item.Ground.bHasContact = true;
		Item.Ground.PlanePointUU = FVector(0.f, 0.f, -PlaneDepthUU);
		Item.Ground.PlaneNormal = FVector::UpVector;
		Item.Ground.Soil = Soil;
		Item.Ground.MountLocalUU = FVector::ZeroVector;
		Item.Ground.AxisLocal = -FVector::ZAxisVector;
		Item.Ground.ForwardLocal = FVector::XAxisVector;
		return Item;
	}

	FWheelBodyView MakeBody()
	{
		FWheelBodyView Body;
		Body.BodyTM = FTransform::Identity;
		Body.LinearVelocityUU = FVector::ZeroVector;
		Body.AngularVelocityRad = FVector::ZeroVector;
		Body.ComWorldUU = FVector::ZeroVector;
		Body.MassKg = RoverMassKg;
		Body.WheelCount = RoverWheelCount;
		// Solid-box inertia for a ~3.5 x 1.5 x 1.0 m hull, in Chaos units
		// (kg*cm^2), so the contact effective mass sees a real roll and pitch
		// response rather than an infinitely stiff body.
		Body.ComRotation = FQuat::Identity;
		Body.InvInertiaDiag = FVector(
			12.f / (RoverMassKg * (1.5f * 1.5f + 1.0f * 1.0f) * 1.0e4f),
			12.f / (RoverMassKg * (3.5f * 3.5f + 1.0f * 1.0f) * 1.0e4f),
			12.f / (RoverMassKg * (3.5f * 3.5f + 1.5f * 1.5f) * 1.0e4f));
		return Body;
	}

	/** Tangential (N) components of an applied force, in the wheel's own frame. */
	float ForceForwardN(const FWheelSimForce& Force)
	{
		return Force.ForceUE.X / ExoneerUnits::NewtonsToUEForce;
	}

	float ForceLateralN(const FWheelSimForce& Force)
	{
		return Force.ForceUE.Y / ExoneerUnits::NewtonsToUEForce;
	}

	/** Run to a settled contact so the radial-drop filter has converged. */
	FWheelSimForce Settle(const FWheelSimInputItem& Item, FWheelBodyView& Body,
		FWheelSimState& State, FWheelSimTelemetry& Telemetry, int32 Steps = 240)
	{
		FWheelSimForce Force;
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			Force = StepWheel(SubstepDt, Item, Body, State, Telemetry);
		}
		return Force;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerWheelParkedTest, "Exoneer.Vehicles.WheelParked",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerWheelParkedTest::RunTest(const FString& Parameters)
{
	const FWheelSimInputItem Item = MakeInput(FirmGroundDefault());
	FWheelBodyView Body = MakeBody();
	FWheelSimState State;
	FWheelSimTelemetry Telemetry;

	// A stationary wheel at zero throttle produces no NET tangential force at
	// all - not a small one that a slope can integrate into a walk.
	const FWheelSimForce Force = Settle(Item, Body, State, Telemetry);
	TestTrue(TEXT("parked wheel is in contact"), Telemetry.bInContact);
	TestTrue(TEXT("parked wheel makes no forward force"), FMath::Abs(ForceForwardN(Force)) < 1.f);
	TestTrue(TEXT("parked wheel makes no lateral force"), FMath::Abs(ForceLateralN(Force)) < 1.f);
	TestTrue(TEXT("parked wheel does not spin"), FMath::Abs(Telemetry.OmegaRadS) < 1e-3f);
	TestTrue(TEXT("parked wheel does not abrade"), Telemetry.ShearForceN * Telemetry.SlipSpeedMS < 1e-3f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerWheelNoCreepTest, "Exoneer.Vehicles.WheelNoCreep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerWheelNoCreepTest::RunTest(const FString& Parameters)
{
	// Report 1: the parked rover drifted sideways. Every tangential term was an
	// odd function of sliding speed through zero, so the contact was a lumped
	// damper stiff enough that the substep could not integrate it - it diverged
	// into a buzz that any asymmetry rectified into a walk. Static friction
	// bounded by the arrest impulse is unconditionally monotone: drive the
	// wheel sideways and it must decay to rest without ever reversing.
	const FWheelSimInputItem Item = MakeInput(FirmGroundDefault());
	FWheelBodyView Body = MakeBody();
	FWheelSimState State;
	FWheelSimTelemetry Telemetry;
	Settle(Item, Body, State, Telemetry);

	float LateralSpeedMS = 0.05f;
	Body.LinearVelocityUU = FVector(0.f, LateralSpeedMS * ExoneerUnits::CmPerM, 0.f);

	float PreviousSpeed = LateralSpeedMS;
	for (int32 Step = 0; Step < 240; ++Step)
	{
		const FWheelSimForce Force = StepWheel(SubstepDt, Item, Body, State, Telemetry);
		// A friction force may stop the sliding within the substep but must
		// never drive it backwards: that is the whole stability argument.
		const float AppliedN = ForceLateralN(Force);
		TestTrue(TEXT("hold never exceeds the arrest impulse"),
			FMath::Abs(AppliedN) <= CornerMassKg * FMath::Abs(LateralSpeedMS) / SubstepDt + 1.f);
		TestTrue(TEXT("hold never exceeds the Mohr-Coulomb budget"),
			FMath::Abs(AppliedN) <= ShearBudget(0.f, Telemetry.NormalLoadN, FirmGroundDefault()) * Item.Config.HardSurfaceGrip + 1.f);

		LateralSpeedMS += AppliedN * SubstepDt / CornerMassKg;
		TestTrue(TEXT("creep never reverses sign"), LateralSpeedMS >= -1e-4f);
		TestTrue(TEXT("creep is monotonically arrested"), LateralSpeedMS <= PreviousSpeed + 1e-6f);
		PreviousSpeed = LateralSpeedMS;
		Body.LinearVelocityUU = FVector(0.f, LateralSpeedMS * ExoneerUnits::CmPerM, 0.f);
	}
	TestTrue(TEXT("parked rover comes to rest"), FMath::Abs(LateralSpeedMS) < 1e-3f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerWheelContactBlinkTest, "Exoneer.Vehicles.WheelContactBlink",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerWheelContactBlinkTest::RunTest(const FString& Parameters)
{
	// Report 5: on the sand field the free-rolling radial drop is tens of
	// millimetres, and the airborne branch used to throw it away. Every probe
	// blink then restarted the filter from a full-radius wheel, which is a
	// ride-height pulse and a load step. Contact must come back where it left.
	FWheelSimInputItem Item = MakeInput(DrySandSoil());
	FWheelBodyView Body = MakeBody();
	FWheelSimState State;
	FWheelSimTelemetry Telemetry;

	Settle(Item, Body, State, Telemetry);
	const float SettledLoadN = Telemetry.NormalLoadN;
	const float SettledDropM = State.PrevSolvedRadialDropM;
	TestTrue(TEXT("sand actually sinks the wheel"), SettledDropM > 0.01f);

	// One frame of lost contact at 60 fps is two substeps; allow a longer blink.
	Item.Ground.bHasContact = false;
	for (int32 Step = 0; Step < 6; ++Step)
	{
		StepWheel(SubstepDt, Item, Body, State, Telemetry);
	}
	TestFalse(TEXT("blink reads as airborne"), Telemetry.bInContact);

	Item.Ground.bHasContact = true;
	const FWheelSimForce Regained = StepWheel(SubstepDt, Item, Body, State, Telemetry);
	TestTrue(TEXT("contact returns"), Telemetry.bInContact);
	TestTrue(TEXT("regained load does not spike"),
		FMath::Abs(Telemetry.NormalLoadN - SettledLoadN) <= 0.02f * SettledLoadN);
	TestTrue(TEXT("regained contact makes no tangential kick"),
		FMath::Abs(ForceForwardN(Regained)) < 5.f && FMath::Abs(ForceLateralN(Regained)) < 5.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerWheelLateralLimitTest, "Exoneer.Vehicles.WheelLateralLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerWheelLateralLimitTest::RunTest(const FString& Parameters)
{
	// Lateral force saturates at the friction limit and never past it, and the
	// hard-surface interface factor scales it exactly once - no double count
	// with the soil-shear mobilisation factor.
	FWheelSimInputItem Item = MakeInput(FirmGroundDefault());
	FWheelBodyView Body = MakeBody();
	FWheelSimState State;
	FWheelSimTelemetry Telemetry;
	Settle(Item, Body, State, Telemetry);

	Body.LinearVelocityUU = FVector(0.f, 5.f * ExoneerUnits::CmPerM, 0.f);
	FWheelSimForce Force;
	for (int32 Step = 0; Step < 60; ++Step)
	{
		Force = StepWheel(SubstepDt, Item, Body, State, Telemetry);
	}
	const float BudgetN = ShearBudget(0.f, Telemetry.NormalLoadN, FirmGroundDefault());
	const float LateralN = FMath::Abs(ForceLateralN(Force));
	TestTrue(TEXT("lateral force never exceeds the budget"), LateralN <= BudgetN * 1.01f);
	TestTrue(TEXT("hard sliding reaches the budget"), LateralN >= BudgetN * 0.9f);
	TestTrue(TEXT("lateral force opposes the slide"), ForceLateralN(Force) < 0.f);

	// A lugged tire stands its carcass off rock and develops less of that same
	// coefficient. The ratio must be exactly the authored factor.
	FWheelSimInputItem Lugged = MakeInput(FirmGroundDefault());
	Lugged.Config.HardSurfaceGrip = 0.72f;
	Lugged.Config.TreadMobilisation = 1.15f;   // must have NO effect on hard ground
	FWheelBodyView LuggedBody = MakeBody();
	FWheelSimState LuggedState;
	FWheelSimTelemetry LuggedTelemetry;
	Settle(Lugged, LuggedBody, LuggedState, LuggedTelemetry);
	LuggedBody.LinearVelocityUU = FVector(0.f, 5.f * ExoneerUnits::CmPerM, 0.f);
	FWheelSimForce LuggedForce;
	for (int32 Step = 0; Step < 60; ++Step)
	{
		LuggedForce = StepWheel(SubstepDt, Lugged, LuggedBody, LuggedState, LuggedTelemetry);
	}
	const float Ratio = FMath::Abs(ForceLateralN(LuggedForce)) / FMath::Max(LateralN, 1.f);
	TestTrue(TEXT("lugs lose grip on hard ground in proportion to the authored factor"),
		FMath::IsNearlyEqual(Ratio, 0.72f, 0.03f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerWheelVisualRestTest, "Exoneer.Vehicles.WheelVisualRest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerWheelVisualRestTest::RunTest(const FString& Parameters)
{
	// Report 6: the drawn wheel must REST on the ground. The mesh centre sits
	// at the solved hub, one static radius above the contact point, and the
	// hub the client reconstructs from the replicated quantum must be the hub
	// the solver produced - not a saturated one 50 mm lower.
	const FWheelSimInputItem Item = MakeInput(FirmGroundDefault());
	const FWheelSimConfig& Config = Item.Config;
	FWheelBodyView Body = MakeBody();
	FWheelSimState State;
	FWheelSimTelemetry Telemetry;
	const FWheelSimForce Force = Settle(Item, Body, State, Telemetry);

	const float VisualTravelM = Config.TravelM + Config.BumpStopTravelM;
	const float SolvedHubZ = -(Config.RestLengthM - Telemetry.VisualCompressionM) * ExoneerUnits::CmPerM;
	const float ContactZ = Force.LocationUU.Z;
	const float StaticRadiusM = (SolvedHubZ - ContactZ) / ExoneerUnits::CmPerM;

	// The contact point is one STATIC radius below the hub, and on firm ground
	// the tire is barely deformed, so that is essentially the full radius.
	TestTrue(TEXT("firm ground barely deforms the tire"),
		Config.RadiusM - StaticRadiusM < 0.005f && StaticRadiusM <= Config.RadiusM);

	// Round-trip the replicated compression exactly as the wire format does.
	const uint8 CompressionQ = (uint8)FMath::Clamp(
		FMath::RoundToInt(Telemetry.VisualCompressionM / VisualTravelM * 255.f), 0, 255);
	const float VisualCompressionM = (CompressionQ / 255.f) * VisualTravelM;
	const float VisualHubZ = -(Config.RestLengthM - VisualCompressionM) * ExoneerUnits::CmPerM;
	const float QuantumUU = VisualTravelM / 255.f * ExoneerUnits::CmPerM;
	TestTrue(TEXT("drawn hub matches the solved hub within one quantum"),
		FMath::Abs(VisualHubZ - SolvedHubZ) <= QuantumUU);

	// The drawn circle touches the ground, sunk only by the real radial drop.
	const float DrawnBottomZ = VisualHubZ - Config.RadiusM * ExoneerUnits::CmPerM;
	const float BuriedUU = ContactZ - DrawnBottomZ;
	TestTrue(TEXT("wheel rests on the ground, not in it"),
		BuriedUU >= -QuantumUU && BuriedUU <= (Config.RadiusM - StaticRadiusM) * ExoneerUnits::CmPerM + QuantumUU);
	TestTrue(TEXT("firm-ground burial is about a millimetre"), BuriedUU < 0.6f);

	// The quantizer must still resolve the deepest compression the solver can
	// reach: saturating early was the visual-only burial at curbs and landings.
	FWheelSimInputItem Bottomed = Item;
	Bottomed.Ground.PlanePointUU = FVector(0.f, 0.f, -20.f);   // ground jammed up under the hub
	FWheelSimState BottomedState;
	FWheelSimTelemetry BottomedTelemetry;
	FWheelBodyView BottomedBody = MakeBody();
	Settle(Bottomed, BottomedBody, BottomedState, BottomedTelemetry, 60);
	TestTrue(TEXT("solver reaches the end of the bump stop"),
		FMath::IsNearlyEqual(BottomedTelemetry.VisualCompressionM, VisualTravelM, 1e-3f));
	const uint8 BottomedQ = (uint8)FMath::Clamp(
		FMath::RoundToInt(BottomedTelemetry.VisualCompressionM / VisualTravelM * 255.f), 0, 255);
	TestEqual(TEXT("full compression maps to the top of the quantizer"), (int32)BottomedQ, 255);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerWheelLaunchTest, "Exoneer.Vehicles.WheelLaunch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerWheelLaunchTest::RunTest(const FString& Parameters)
{
	// Report 4. Firm ground has the grip to put the motor's torque down without
	// wheelspin; clay does not, and must still punish raw throttle. Same code,
	// same throttle, two soils - the difference is the Mohr-Coulomb budget.
	auto RunLaunch = [](const FSoilParams& Soil, float& OutSpeedMS, float& OutPeakSlipMS)
	{
		FWheelSimInputItem Item = MakeInput(Soil);
		Item.Command.Throttle = 1.f;
		Item.Command.bParkingBrake = false;
		FWheelBodyView Body = MakeBody();
		FWheelSimState State;
		FWheelSimTelemetry Telemetry;

		float SpeedMS = 0.f;
		OutPeakSlipMS = 0.f;
		for (int32 Step = 0; Step < 240; ++Step)   // 2 s
		{
			const FWheelSimForce Force = StepWheel(SubstepDt, Item, Body, State, Telemetry);
			SpeedMS += ForceForwardN(Force) * SubstepDt / CornerMassKg;
			Body.LinearVelocityUU = FVector(SpeedMS * ExoneerUnits::CmPerM, 0.f, 0.f);
			if (Step > 24)   // ignore the first 200 ms of filter settling
			{
				OutPeakSlipMS = FMath::Max(OutPeakSlipMS, Telemetry.SlipSpeedMS);
			}
		}
		OutSpeedMS = SpeedMS;
	};

	float FirmSpeed = 0.f, FirmSlip = 0.f;
	RunLaunch(FirmGroundDefault(), FirmSpeed, FirmSlip);
	TestTrue(TEXT("firm ground actually accelerates the rover"), FirmSpeed > 2.f);
	TestTrue(TEXT("firm ground puts the torque down without wheelspin"), FirmSlip < 0.2f);

	float ClaySpeed = 0.f, ClaySlip = 0.f;
	RunLaunch(ClaySoil(), ClaySpeed, ClaySlip);
	TestTrue(TEXT("clay still spins under raw throttle"), ClaySlip > 0.5f);
	TestTrue(TEXT("clay accelerates worse than firm ground"), ClaySpeed < FirmSpeed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerWheelVisualSinkageSplitTest, "Exoneer.Vehicles.WheelVisualSinkageSplit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerWheelVisualSinkageSplitTest::RunTest(const FString& Parameters)
{
	// Report 6, second pass: folding sinkage into the ONE radial drop lowered
	// the drawn hub by the whole rut - about 53 mm on sand - which is exactly
	// the "wheel is buried in the ground" the owner reported. The solve must
	// keep the rut (compaction, bulldozing and rut drag are charged against
	// it) while the drawn hub keeps only the tire squash, until ruts render.
	FWheelSimInputItem Sand = MakeInput(DrySandSoil());
	FWheelBodyView SandBody = MakeBody();
	FWheelSimState SandState;
	FWheelSimTelemetry SandTelemetry;
	Settle(Sand, SandBody, SandState, SandTelemetry);

	TestTrue(TEXT("sand actually sinks the wheel"), SandTelemetry.SinkageM > 0.005f);
	TestTrue(TEXT("the solved radial drop carries the sinkage"),
		FMath::IsNearlyEqual(SandState.PrevSolvedRadialDropM - SandState.PrevVisualRadialDropM,
			SandTelemetry.SinkageM, 0.002f));
	TestTrue(TEXT("the visual radial drop carries none of it"),
		SandState.PrevVisualRadialDropM < SandState.PrevSolvedRadialDropM - 0.005f);

	// Larger compression means a HIGHER hub, so the drawn hub sits above the
	// solved one by exactly the rut the solver put the wheel in.
	TestTrue(TEXT("the drawn hub rides above the solved hub by the sinkage"),
		FMath::IsNearlyEqual(SandTelemetry.VisualCompressionM - SandState.SolvedCompressionM,
			SandTelemetry.SinkageM, 0.002f));
	TestTrue(TEXT("the drawn wheel is not lowered into the rut"),
		SandTelemetry.VisualCompressionM > SandState.SolvedCompressionM);

	// Firm ground has no rut to leave, so there is nothing to split: the two
	// must stay the same number, or this split would have moved the wheel on
	// every surface instead of only on soil.
	FWheelSimInputItem Firm = MakeInput(FirmGroundDefault());
	FWheelBodyView FirmBody = MakeBody();
	FWheelSimState FirmState;
	FWheelSimTelemetry FirmTelemetry;
	Settle(Firm, FirmBody, FirmState, FirmTelemetry);

	TestTrue(TEXT("firm ground barely sinks"), FirmTelemetry.SinkageM < 0.001f);
	TestTrue(TEXT("on firm ground the two radial drops agree within a millimetre"),
		FMath::Abs(FirmState.PrevSolvedRadialDropM - FirmState.PrevVisualRadialDropM) < 0.001f);
	TestTrue(TEXT("on firm ground the two compressions agree within a millimetre"),
		FMath::Abs(FirmTelemetry.VisualCompressionM - FirmState.SolvedCompressionM) < 0.001f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
