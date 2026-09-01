// Copyright Exoneer contributors.
//
// Unit tests for the pure terramechanics library. Expected values come from an
// independent double-precision reference implementation (bisection sinkage
// solver, not Newton), mirroring docs/design/wheels/design-math-spec.md
// section 8 - test vectors T1..T11 plus the critique's replacements.
//
// Headless run:
//   UnrealEditor-Cmd.exe <uproject> -ExecCmds="Automation RunTests Exoneer.Terramechanics; Quit"
//     -TestExit="Automation Test Queue Empty" -unattended -nullrhi -nosplash -nop4 -log

#include "Misc/AutomationTest.h"
#include "Vehicles/ExoneerTerramechanics.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	using namespace ExoneerTerramechanics;

	// Wong's soil table, SI-converted (spec section 2).
	FSoilParams DrySand()
	{
		FSoilParams Soil;
		Soil.Kc = 990.f; Soil.Kphi = 1528430.f; Soil.N0 = 1.1f; Soil.N1 = 0.9f;
		Soil.Cohesion = 1040.f; Soil.FrictionAngleRad = FMath::DegreesToRadians(28.f);
		Soil.ShearK = 0.025f; Soil.ShearKy = 0.030f; Soil.UnitWeight = 15700.f;
		return Soil;
	}

	FSoilParams SandyLoam()
	{
		FSoilParams Soil;
		Soil.Kc = 5270.f; Soil.Kphi = 1515040.f; Soil.N0 = 0.7f; Soil.N1 = 0.6f;
		Soil.Cohesion = 1720.f; Soil.FrictionAngleRad = FMath::DegreesToRadians(29.f);
		Soil.ShearK = 0.025f; Soil.ShearKy = 0.030f; Soil.UnitWeight = 15200.f;
		return Soil;
	}

	FSoilParams ClayeySoil()
	{
		FSoilParams Soil;
		Soil.Kc = 13190.f; Soil.Kphi = 692150.f; Soil.N0 = 0.5f; Soil.N1 = 0.4f;
		Soil.Cohesion = 4140.f; Soil.FrictionAngleRad = FMath::DegreesToRadians(13.f);
		Soil.ShearK = 0.010f; Soil.ShearKy = 0.012f; Soil.UnitWeight = 16800.f;
		return Soil;
	}

	// Common test wheel: r = 0.3 m, b = 0.2 m, W = 3000 N.
	constexpr float TestRadius = 0.3f;
	constexpr float TestWidth = 0.2f;
	constexpr float TestLoad = 3000.f;

	bool NearRel(FAutomationTestBase& Test, const TCHAR* What, float Actual, float Expected, float RelTolerance = 2e-3f)
	{
		const float Tolerance = FMath::Max(FMath::Abs(Expected) * RelTolerance, 1e-6f);
		if (!FMath::IsNearlyEqual(Actual, Expected, Tolerance))
		{
			Test.AddError(FString::Printf(TEXT("%s: got %.6f, expected %.6f (rel tol %.4f)"), What, Actual, Expected, RelTolerance));
			return false;
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerTerraSinkageTest, "Exoneer.Terramechanics.Sinkage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerTerraSinkageTest::RunTest(const FString& Parameters)
{
	const FSoilParams Sand = DrySand();
	const FSoilParams Clay = ClayeySoil();

	// T1: rigid solve, dry sand.
	const float ZSand = SolveRigidSinkage(TestLoad, TestWidth, TestRadius, Sand);
	NearRel(*this, TEXT("T1 sand sinkage"), ZSand, 0.067540f);
	NearRel(*this, TEXT("T1 patch length"), ChordPatchLength(ZSand, TestRadius), 0.189637f);
	// Bekker cross-check: solved z satisfies p = k_eq * z^n at the patch pressure.
	const float Patch = ChordPatchLength(ZSand, TestRadius);
	const float Pressure = TestLoad / (TestWidth * Patch);
	NearRel(*this, TEXT("T1 Bekker consistency"), KEq(Sand, TestWidth) * FMath::Pow(ZSand, 1.1f), Pressure);

	// T2: clay sinks far less at equal load (low exponent stiffens shallow response).
	const float ZClay = SolveRigidSinkage(TestLoad, TestWidth, TestRadius, Clay);
	NearRel(*this, TEXT("T2 clay sinkage"), ZClay, 0.026119f);
	TestTrue(TEXT("T2 clay < sand"), ZClay < ZSand);

	// Monotonic in load.
	const float ZHeavy = SolveRigidSinkage(6000.f, TestWidth, TestRadius, Sand);
	NearRel(*this, TEXT("T3 sand sinkage at 6 kN"), ZHeavy, 0.106676f);
	TestTrue(TEXT("z monotonic in W"), ZHeavy > ZSand);

	// T9: slip sinkage - spinning wheels dig in.
	const float ZSpin = SolveRigidSinkage(TestLoad, TestWidth, TestRadius, Sand, 0.8f);
	NearRel(*this, TEXT("T9 sinkage at s=0.8"), ZSpin, 0.162636f);
	TestTrue(TEXT("z monotonic in slip"), ZSpin > ZSand * 2.f);

	// Firm-ground fallback: millimeter sinkage, graceful degradation to rigid pavement.
	const float ZFirm = SolveRigidSinkage(TestLoad, TestWidth, TestRadius, FirmGroundDefault());
	NearRel(*this, TEXT("firm ground sinkage"), ZFirm, 0.00114544f, 5e-3f);

	// No load, no contact.
	TestEqual(TEXT("W=0 gives z=0"), SolveRigidSinkage(0.f, TestWidth, TestRadius, Sand), 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerTerraResistanceTest, "Exoneer.Terramechanics.Resistance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerTerraResistanceTest::RunTest(const FString& Parameters)
{
	const FSoilParams Sand = DrySand();

	// T3: compaction resistance grows superlinearly with load ("heavy cargo explodes R").
	const float RcLight = CompactionResistance(TestWidth, 0.067540f, Sand);
	const float RcHeavy = CompactionResistance(TestWidth, 0.106676f, Sand);
	NearRel(*this, TEXT("T3 Rc at 3 kN"), RcLight, 508.79f);
	NearRel(*this, TEXT("T3 Rc at 6 kN"), RcHeavy, 1328.61f);
	TestTrue(TEXT("T3 doubling W more than doubles Rc"), RcHeavy / RcLight > 2.f);

	// T5: Rankine bulldozing.
	NearRel(*this, TEXT("T5 bulldozing"), BulldozingResistance(TestWidth, 0.067540f, Sand), 66.598f);

	// T9: slip raises total resistance (n_eff feeds Rc; z from the slip-sinkage solve).
	const float RcSpin = CompactionResistance(TestWidth, 0.162636f, Sand, 0.8f);
	const float RbSpin = BulldozingResistance(TestWidth, 0.162636f, Sand);
	NearRel(*this, TEXT("T9 Rc at s=0.8"), RcSpin, 648.72f);
	NearRel(*this, TEXT("T9 Rb at s=0.8"), RbSpin, 227.62f);
	TestTrue(TEXT("T9 resistance grows with slip"), RcSpin + RbSpin > RcLight + 66.6f);

	// Both terms vanish continuously at z = 0.
	TestEqual(TEXT("Rc(0) = 0"), CompactionResistance(TestWidth, 0.f, Sand), 0.f);
	TestEqual(TEXT("Rb(0) = 0"), BulldozingResistance(TestWidth, 0.f, Sand), 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerTerraTractionTest, "Exoneer.Terramechanics.Traction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerTerraTractionTest::RunTest(const FString& Parameters)
{
	const FSoilParams Sand = DrySand();
	// T1 patch on dry sand.
	const float Patch = 0.189637f;
	const float Area = TestWidth * Patch;
	const float Budget = ShearBudget(Area, TestLoad, Sand);
	NearRel(*this, TEXT("T4 shear budget"), Budget, 1634.57f);

	// T4: gross tractive effort at three slips. Per the critique (B4): F is
	// monotone RISING in s and bounded by the budget; what falls with slip is
	// drawbar pull once slip sinkage raises R (covered in Resistance).
	const float F05 = CombinedShearForces(Area, TestLoad, 0.05f, 0.f, Patch, Sand).LongitudinalN;
	const float F20 = CombinedShearForces(Area, TestLoad, 0.20f, 0.f, Patch, Sand).LongitudinalN;
	const float F80 = CombinedShearForces(Area, TestLoad, 0.80f, 0.f, Patch, Sand).LongitudinalN;
	NearRel(*this, TEXT("T4 F at s=0.05"), F05, 274.24f);
	NearRel(*this, TEXT("T4 F at s=0.20"), F20, 793.47f);
	NearRel(*this, TEXT("T4 F at s=0.80"), F80, 1365.84f);
	TestTrue(TEXT("F monotone rising in s"), F05 < F20 && F20 < F80);
	TestTrue(TEXT("F bounded by budget"), F80 < Budget);

	// s -> 0 limit: force vanishes; series branch continuous at the switch
	// (0.02 - the float32 cancellation guard, see ShearSaturation).
	TestEqual(TEXT("F(0) = 0"), CombinedShearForces(Area, TestLoad, 0.f, 0.f, Patch, Sand).LongitudinalN, 0.f);
	NearRel(*this, TEXT("E(u) series side of switch"), ShearSaturation(0.0199f), 0.00988433f);
	NearRel(*this, TEXT("E(u) full side of switch"), ShearSaturation(0.0201f), 0.00998300f);
	NearRel(*this, TEXT("E(u) mid-range"), ShearSaturation(0.5f), 0.21306132f);
	TestTrue(TEXT("E(u) monotonic across switch"), ShearSaturation(0.0199f) < ShearSaturation(0.0201f));

	// T10: combined slip shares one budget; steering steals traction.
	const FShearForces Combined = CombinedShearForces(Area, TestLoad, 0.20f, FMath::DegreesToRadians(8.f), Patch, Sand);
	NearRel(*this, TEXT("T10 longitudinal"), Combined.LongitudinalN, 746.51f);
	NearRel(*this, TEXT("T10 lateral"), Combined.LateralN, -437.15f);
	NearRel(*this, TEXT("T10 resultant"), Combined.ResultantN, 865.09f);
	NearRel(*this, TEXT("T10 friction ellipse identity"),
		FMath::Sqrt(FMath::Square(Combined.LongitudinalN) + FMath::Square(Combined.LateralN)), Combined.ResultantN);
	TestTrue(TEXT("T10 resultant within budget"), Combined.ResultantN <= Budget);
	TestTrue(TEXT("T10 steering reduces longitudinal force"), Combined.LongitudinalN < F20);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerTerraSlipTest, "Exoneer.Terramechanics.Slip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerTerraSlipTest::RunTest(const FString& Parameters)
{
	// T6: regularization - no NaN, no force at rest, softened near rest, SAE away from it.
	TestEqual(TEXT("T6 at rest"), SlipRatio(0.f, 0.f), 0.f);
	NearRel(*this, TEXT("T6 creep spin"), SlipRatio(0.05f, 0.f), 0.5f);
	NearRel(*this, TEXT("T6 braking branch"), SlipRatio(1.f, 2.f), -0.5f);
	NearRel(*this, TEXT("T6 driving branch, SAE"), SlipRatio(2.f, 1.f), 0.5f);
	TestEqual(TEXT("T6 clamp at full spin"), SlipRatio(50.f, 0.f), 1.f);

	// Slip angle regularization.
	TestEqual(TEXT("alpha at rest"), SlipAngle(0.f, 0.f), 0.f);
	NearRel(*this, TEXT("alpha at speed"), SlipAngle(1.f, 5.f), FMath::Atan(0.2f));

	// Governor (Shear Control talent): 1 in the optimal window, linear cut above.
	TestEqual(TEXT("governor below target"), SlipGovernor(0.10f), 1.f);
	NearRel(*this, TEXT("governor mid-cut"), SlipGovernor(0.25f), 0.5f);
	TestEqual(TEXT("governor full cut"), SlipGovernor(0.35f), 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerTerraRegimeTest, "Exoneer.Terramechanics.Regime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerTerraRegimeTest::RunTest(const FString& Parameters)
{
	const FSoilParams Loam = SandyLoam();

	// T8: Wong's criterion on sandy loam at 3 kN.
	const float CriticalPressure = CriticalGroundPressure(TestLoad, TestWidth, TestRadius, Loam);
	NearRel(*this, TEXT("T8 p_gcr"), CriticalPressure, 140077.6f);

	// 80 kPa inflation + 15 kPa carcass < p_gcr: flexible regime, CTIS territory.
	const FWheelContactSolution AtNominal = SolveWheelContact(TestLoad, TestWidth, TestRadius, 80000.f, 15000.f, Loam, 0.f, true);
	TestFalse(TEXT("T8 flexible at 95 kPa"), AtNominal.bRigid);
	NearRel(*this, TEXT("T8 flexible sinkage"), AtNominal.SinkageM, 0.018671f);
	NearRel(*this, TEXT("T8 patch length"), AtNominal.PatchLengthM, 0.157895f);
	NearRel(*this, TEXT("T8 deflection"), AtNominal.DeflectionM, 0.010574f);
	NearRel(*this, TEXT("T8 effective radius"), AtNominal.EffectiveRadiusM, 0.296475f);
	NearRel(*this, TEXT("T8 Rc at 95 kPa"), CompactionResistance(TestWidth, AtNominal.SinkageM, Loam), 208.67f);

	// CTIS drop to 0.6x inflation: sinkage -44 percent, compaction resistance -63 percent.
	const FWheelContactSolution AtLowPressure = SolveWheelContact(TestLoad, TestWidth, TestRadius, 48000.f, 15000.f, Loam, 0.f, false);
	TestFalse(TEXT("T8 still flexible at 63 kPa"), AtLowPressure.bRigid);
	NearRel(*this, TEXT("T8 CTIS sinkage"), AtLowPressure.SinkageM, 0.010383f);
	NearRel(*this, TEXT("T8 CTIS Rc"), CompactionResistance(TestWidth, AtLowPressure.SinkageM, Loam), 76.957f);

	// Hysteresis: 2 percent band around p_gcr, no flip-flapping.
	const float JustBelow = CriticalPressure * 1.01f;
	TestFalse(TEXT("hysteresis holds flexible inside the band"),
		SolveWheelContact(TestLoad, TestWidth, TestRadius, JustBelow, 0.f, Loam, 0.f, false).bRigid);
	TestTrue(TEXT("hysteresis holds rigid inside the band"),
		SolveWheelContact(TestLoad, TestWidth, TestRadius, CriticalPressure * 0.99f, 0.f, Loam, 0.f, true).bRigid);
	TestTrue(TEXT("clearly above the band goes rigid"),
		SolveWheelContact(TestLoad, TestWidth, TestRadius, CriticalPressure * 1.05f, 0.f, Loam, 0.f, false).bRigid);

	// No load: contact-free solution, effective radius intact.
	const FWheelContactSolution Airborne = SolveWheelContact(0.f, TestWidth, TestRadius, 80000.f, 15000.f, Loam, 0.f, true);
	TestEqual(TEXT("airborne sinkage"), Airborne.SinkageM, 0.f);
	TestEqual(TEXT("airborne effective radius"), Airborne.EffectiveRadiusM, TestRadius);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerTerraMotorTest, "Exoneer.Terramechanics.Motor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerTerraMotorTest::RunTest(const FString& Parameters)
{
	// T7: PMDC curve, T_s = 250 Nm, omega_0 = 40 rad/s, eta = 0.85, P_cu0 = 2000 W.
	const float StallTorque = MotorTorque(250.f, 40.f, 0.f, 1.f);
	NearRel(*this, TEXT("T7 stall torque"), StallTorque, 250.f);
	NearRel(*this, TEXT("T7 stall draw is pure copper loss"),
		MotorElectricalPower(StallTorque, 0.f, 250.f, 0.85f, 2000.f), 2000.f);

	const float NearNoLoadTorque = MotorTorque(250.f, 40.f, 36.f, 1.f);
	NearRel(*this, TEXT("T7 torque near no-load"), NearNoLoadTorque, 25.f);
	NearRel(*this, TEXT("T7 draw near no-load"),
		MotorElectricalPower(NearNoLoadTorque, 36.f, 250.f, 0.85f, 2000.f), 1078.82f);

	TestEqual(TEXT("T7 no-load torque"), MotorTorque(250.f, 40.f, 40.f, 1.f), 0.f);

	// Plugging cap: reverse command against forward rotation cannot exceed stall torque.
	NearRel(*this, TEXT("plugging capped at stall"), MotorTorque(250.f, 40.f, 10.f, -1.f), -250.f);
	// Reverse drive mirrors forward.
	NearRel(*this, TEXT("reverse curve"), MotorTorque(250.f, 40.f, -36.f, -1.f), -25.f);
	// Brown-out scales torque.
	NearRel(*this, TEXT("supply fraction scales torque"), MotorTorque(250.f, 40.f, 0.f, 1.f, 0.5f), 125.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerTerraSuspensionTest, "Exoneer.Terramechanics.Suspension",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerTerraSuspensionTest::RunTest(const FString& Parameters)
{
	// T11: strut force and the no-pull clamp.
	NearRel(*this, TEXT("T11 compressing strut"), SuspensionForce(40000.f, 3162.f, 0.05f, -0.2f, 0.20f), 1367.6f);
	TestEqual(TEXT("T11 rebound clamped to zero"), SuspensionForce(40000.f, 3162.f, 0.005f, -3.f, 0.20f), 0.f);
	// Bump stop engages past full travel.
	NearRel(*this, TEXT("bump stop"), SuspensionForce(40000.f, 3162.f, 0.22f, 0.f, 0.20f, 400000.f), 16800.f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
