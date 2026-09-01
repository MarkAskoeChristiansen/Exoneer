// Copyright Exoneer contributors.
#include "Vehicles/ExoneerTerramechanics.h"

namespace ExoneerTerramechanics
{

FSoilParams FirmGroundDefault(float CoulombMu)
{
	FSoilParams Soil;
	Soil.Kc = 0.f;
	Soil.Kphi = 5.0e8f;   // near-rigid: ~1 mm sinkage at rover wheel loads
	Soil.N0 = 1.0f;
	Soil.N1 = 0.f;        // hard ground does not dig in
	Soil.Cohesion = 0.f;
	Soil.FrictionAngleRad = FMath::Atan(FMath::Max(CoulombMu, 0.05f));
	Soil.ShearK = 0.01f;
	Soil.ShearKy = 0.012f;
	Soil.UnitWeight = 18000.f;
	return Soil;
}

float KEq(const FSoilParams& Soil, float WidthM)
{
	return Soil.Kc / FMath::Max(WidthM, 0.01f) + Soil.Kphi;
}

float EffectiveExponent(const FSoilParams& Soil, float SlipRatioAbs)
{
	return FMath::Clamp(Soil.N0 + Soil.N1 * FMath::Abs(SlipRatioAbs), 0.2f, 2.5f);
}

float ChordPatchLength(float SinkageM, float RadiusM)
{
	const float Z = FMath::Clamp(SinkageM, 0.f, 0.95f * RadiusM);
	return FMath::Sqrt(FMath::Max(Z * (2.f * RadiusM - Z), 0.f));
}

float SolveRigidSinkage(float NormalLoadN, float WidthM, float RadiusM, const FSoilParams& Soil, float SlipRatioAbs)
{
	if (NormalLoadN <= 0.f || WidthM <= KINDA_SMALL_NUMBER || RadiusM <= KINDA_SMALL_NUMBER)
	{
		return 0.f;
	}

	const float Ke = KEq(Soil, WidthM);
	const float NEff = EffectiveExponent(Soil, SlipRatioAbs);
	const float ZMin = 1e-6f;
	const float ZMax = 0.95f * RadiusM;

	// Closed-form seed from the small-sinkage patch l ~ sqrt(2 r z).
	float Z = FMath::Pow(NormalLoadN / (WidthM * Ke * FMath::Sqrt(2.f * RadiusM)), 1.f / (NEff + 0.5f));
	Z = FMath::Clamp(Z, ZMin, ZMax);

	for (int32 Iteration = 0; Iteration < 8; ++Iteration)
	{
		const float Chord = ChordPatchLength(Z, RadiusM);
		const float ZPowN = FMath::Pow(Z, NEff);
		const float G = WidthM * Ke * ZPowN * Chord - NormalLoadN;
		const float DChord = (RadiusM - Z) / FMath::Max(Chord, 1e-4f);
		const float DG = WidthM * Ke * (NEff * FMath::Pow(Z, NEff - 1.f) * Chord + ZPowN * DChord);
		if (DG < 1e-3f)
		{
			break;
		}
		const float Step = G / DG;
		Z = FMath::Clamp(Z - Step, ZMin, ZMax);
		if (FMath::Abs(Step) < 1e-5f)
		{
			break;
		}
	}
	return Z;
}

float CriticalGroundPressure(float NormalLoadN, float WidthM, float RadiusM, const FSoilParams& Soil, float SlipRatioAbs)
{
	if (NormalLoadN <= 0.f || WidthM <= KINDA_SMALL_NUMBER || RadiusM <= KINDA_SMALL_NUMBER)
	{
		return 0.f;
	}
	const float NEff = EffectiveExponent(Soil, SlipRatioAbs);
	const float Ke = KEq(Soil, WidthM);
	const float Diameter = 2.f * RadiusM;
	const float Denominator = FMath::Max(3.f - NEff, 0.2f) * WidthM * FMath::Sqrt(Diameter);
	return FMath::Pow(Ke, 1.f / (2.f * NEff + 1.f)) * FMath::Pow(3.f * NormalLoadN / Denominator, 2.f * NEff / (2.f * NEff + 1.f));
}

float FlexibleSinkage(float GroundPressurePa, float WidthM, const FSoilParams& Soil, float SlipRatioAbs)
{
	if (GroundPressurePa <= 0.f)
	{
		return 0.f;
	}
	const float NEff = EffectiveExponent(Soil, SlipRatioAbs);
	return FMath::Pow(GroundPressurePa / KEq(Soil, WidthM), 1.f / NEff);
}

float ShearSaturation(float U)
{
	if (U < 0.f)
	{
		return 0.f;
	}
	// The spec's switch point of 1e-3 assumes double precision; in float32
	// the full branch suffers catastrophic cancellation in (1 - e^-u) up to
	// ~u = 0.01 (absolute error ~ulp(1)/u). Switch at 0.02 with a third
	// series term: both branches then agree to ~1e-7 relative at the seam.
	if (U < 0.02f)
	{
		return U * 0.5f - U * U / 6.f + U * U * U / 24.f;
	}
	return 1.f - (1.f - FMath::Exp(-U)) / U;
}

float ShearBudget(float ContactAreaM2, float NormalLoadN, const FSoilParams& Soil)
{
	return ContactAreaM2 * Soil.Cohesion + NormalLoadN * FMath::Tan(Soil.FrictionAngleRad);
}

FShearForces CombinedShearForces(float ContactAreaM2, float NormalLoadN, float SlipRatio, float SlipAngleRad, float PatchLengthM, const FSoilParams& Soil)
{
	FShearForces Out;
	if (ContactAreaM2 <= 0.f || NormalLoadN <= 0.f || PatchLengthM <= 0.f)
	{
		return Out;
	}

	const float ULong = FMath::Abs(SlipRatio) * PatchLengthM / FMath::Max(Soil.ShearK, 1e-4f);
	const float ULat = FMath::Abs(FMath::Tan(SlipAngleRad)) * PatchLengthM / FMath::Max(Soil.ShearKy, 1e-4f);
	const float UResultant = FMath::Sqrt(ULong * ULong + ULat * ULat);
	if (UResultant < 1e-6f)
	{
		return Out;
	}

	const float FResultant = ShearBudget(ContactAreaM2, NormalLoadN, Soil) * ShearSaturation(UResultant);
	Out.ResultantN = FResultant;
	Out.LongitudinalN = FResultant * (ULong / UResultant) * FMath::Sign(SlipRatio);
	Out.LateralN = -FResultant * (ULat / UResultant) * FMath::Sign(SlipAngleRad);
	return Out;
}

float CompactionResistance(float WidthM, float SinkageM, const FSoilParams& Soil, float SlipRatioAbs)
{
	if (SinkageM <= 0.f)
	{
		return 0.f;
	}
	const float NEff = EffectiveExponent(Soil, SlipRatioAbs);
	return WidthM * KEq(Soil, WidthM) * FMath::Pow(SinkageM, NEff + 1.f) / (NEff + 1.f);
}

float BulldozingResistance(float WidthM, float SinkageM, const FSoilParams& Soil)
{
	if (SinkageM <= 0.f)
	{
		return 0.f;
	}
	const float PassiveKp = FMath::Square(FMath::Tan(PI / 4.f + Soil.FrictionAngleRad * 0.5f));
	return 0.5f * Soil.UnitWeight * SinkageM * SinkageM * WidthM * PassiveKp
		+ 2.f * Soil.Cohesion * SinkageM * WidthM * FMath::Sqrt(PassiveKp);
}

float SlipRatio(float WheelSurfaceSpeedMS, float LongitudinalSpeedMS, float EpsMS)
{
	const float Denominator = FMath::Max3(FMath::Abs(WheelSurfaceSpeedMS), FMath::Abs(LongitudinalSpeedMS), EpsMS);
	return FMath::Clamp((WheelSurfaceSpeedMS - LongitudinalSpeedMS) / Denominator, -1.f, 1.f);
}

float SlipAngle(float LateralSpeedMS, float LongitudinalSpeedMS, float EpsMS)
{
	return FMath::Atan(LateralSpeedMS / FMath::Max(FMath::Abs(LongitudinalSpeedMS), EpsMS));
}

float MotorTorque(float StallTorqueNm, float NoLoadSpeedRadS, float OmegaRadS, float Command01, float SupplyFraction)
{
	if (Command01 == 0.f || StallTorqueNm <= 0.f || NoLoadSpeedRadS <= KINDA_SMALL_NUMBER)
	{
		return 0.f;
	}
	const float CommandSign = Command01 > 0.f ? 1.f : -1.f;
	// Lower clamp caps plugging (torque against rotation) at stall torque;
	// linear extrapolation outside [0, omega_0] is non-physical.
	const float CurveFraction = FMath::Clamp(1.f - (OmegaRadS * CommandSign) / NoLoadSpeedRadS, 0.f, 1.f);
	return FMath::Abs(Command01) * StallTorqueNm * CurveFraction * FMath::Clamp(SupplyFraction, 0.f, 1.f) * CommandSign;
}

float MotorElectricalPower(float DriveTorqueNm, float OmegaRadS, float StallTorqueNm, float Efficiency, float CopperLossAtStallW)
{
	const float MechanicalW = FMath::Max(DriveTorqueNm * OmegaRadS, 0.f) / FMath::Max(Efficiency, 0.1f);
	const float TorqueFraction = StallTorqueNm > KINDA_SMALL_NUMBER ? DriveTorqueNm / StallTorqueNm : 0.f;
	return MechanicalW + CopperLossAtStallW * TorqueFraction * TorqueFraction;
}

float SuspensionForce(float SpringNPerM, float DamperNSecPerM, float CompressionM, float CompressionRateMS, float TravelM, float BumpStopNPerM)
{
	float Force = FMath::Max(0.f, SpringNPerM * CompressionM + DamperNSecPerM * CompressionRateMS);
	if (CompressionM > TravelM)
	{
		Force += BumpStopNPerM * (CompressionM - TravelM);
	}
	return Force;
}

float SlipGovernor(float SlipRatioAbs, float TargetSlip, float FullCutSlip)
{
	const float Slip = FMath::Abs(SlipRatioAbs);
	if (Slip <= TargetSlip)
	{
		return 1.f;
	}
	return FMath::Clamp((FullCutSlip - Slip) / FMath::Max(FullCutSlip - TargetSlip, 1e-3f), 0.f, 1.f);
}

FWheelContactSolution SolveWheelContact(float NormalLoadN, float WidthM, float RadiusM, float TirePressurePa, float CarcassPressurePa, const FSoilParams& Soil, float PrevSlipRatioAbs, bool bPrevRigid)
{
	FWheelContactSolution Solution;
	Solution.EffectiveRadiusM = RadiusM;
	Solution.bRigid = bPrevRigid;
	if (NormalLoadN <= 0.f || WidthM <= KINDA_SMALL_NUMBER || RadiusM <= KINDA_SMALL_NUMBER)
	{
		return Solution;
	}

	const float TirePressureTotal = TirePressurePa + CarcassPressurePa;
	const float CriticalPressure = CriticalGroundPressure(NormalLoadN, WidthM, RadiusM, Soil, PrevSlipRatioAbs);
	// 2 percent hysteresis prevents flip-flapping at the boundary.
	const float Threshold = bPrevRigid ? CriticalPressure * 0.98f : CriticalPressure * 1.02f;
	Solution.bRigid = TirePressureTotal >= Threshold;

	const float PatchFloor = 0.01f * RadiusM;
	if (Solution.bRigid)
	{
		Solution.SinkageM = SolveRigidSinkage(NormalLoadN, WidthM, RadiusM, Soil, PrevSlipRatioAbs);
		Solution.PatchLengthM = FMath::Max(ChordPatchLength(Solution.SinkageM, RadiusM), PatchFloor);
		Solution.ContactAreaM2 = WidthM * Solution.PatchLengthM;
		Solution.GroundPressurePa = NormalLoadN / Solution.ContactAreaM2;
		Solution.DeflectionM = 0.f;
		Solution.EffectiveRadiusM = RadiusM;
	}
	else
	{
		// The tire flattens until its contact pressure equals what it can
		// carry: ground pressure is inflation + carcass. This is where CTIS acts.
		Solution.GroundPressurePa = TirePressureTotal;
		Solution.SinkageM = FlexibleSinkage(TirePressureTotal, WidthM, Soil, PrevSlipRatioAbs);
		Solution.ContactAreaM2 = NormalLoadN / TirePressureTotal;
		Solution.PatchLengthM = FMath::Clamp(Solution.ContactAreaM2 / WidthM, PatchFloor, 1.4f * RadiusM);
		const float HalfPatch = FMath::Min(Solution.PatchLengthM * 0.5f, 0.999f * RadiusM);
		Solution.DeflectionM = FMath::Clamp(RadiusM - FMath::Sqrt(RadiusM * RadiusM - HalfPatch * HalfPatch), 0.f, 0.35f * RadiusM);
		Solution.EffectiveRadiusM = RadiusM - Solution.DeflectionM / 3.f;
	}
	return Solution;
}

} // namespace ExoneerTerramechanics
