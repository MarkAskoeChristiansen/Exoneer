// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"

/**
 * Pure terramechanics math: Bekker pressure-sinkage, Wong's rigid/pneumatic
 * regime criterion, Janosi-Hanamoto shear with combined slip by resultant
 * shear displacement, compaction and bulldozing resistance, PMDC hub motor,
 * suspension strut.
 *
 * Everything here is SI: meter, Newton, Pascal, radian, Watt. No UObject, no
 * engine state - every function is a pure function of its arguments and is
 * covered by Tests/ExoneerTerramechanicsTests.cpp.
 *
 * Spec: docs/design/wheels/design-math-spec.md, with the binding corrections
 * from docs/design/wheels/design-critique.md.
 */
namespace ExoneerTerramechanics
{
	/** Substrate constants, SI at runtime (authored assets carry published kN/kPa/deg units and convert once on load). */
	struct FSoilParams
	{
		float Kc = 990.f;                 // N/m^(n+1)   cohesive sinkage modulus
		float Kphi = 1528430.f;           // N/m^(n+2)   frictional sinkage modulus
		float N0 = 1.1f;                  // sinkage exponent at zero slip
		float N1 = 0.9f;                  // slip-sinkage coefficient: n_eff = N0 + N1*|s|
		float Cohesion = 1040.f;          // Pa
		float FrictionAngleRad = 0.48869f;// rad (28 deg = Wong's LLL dry sand)
		float ShearK = 0.025f;            // m, longitudinal Janosi shear deformation modulus
		float ShearKy = 0.030f;           // m, lateral shear deformation modulus
		float UnitWeight = 15700.f;       // N/m^3, feeds bulldozing resistance
	};

	/**
	 * Near-rigid ground for hits that carry no soil physical material.
	 * Sinkage collapses toward ~1 mm and traction becomes Coulomb-limited
	 * through the same Janosi expression (tan(phi) = CoulombMu) - one code
	 * path, no special-case friction curve.
	 */
	FSoilParams FirmGroundDefault(float CoulombMu = 0.7f);

	/** k_eq = k_c / b + k_phi. */
	float KEq(const FSoilParams& Soil, float WidthM);

	/** n_eff = N0 + N1 * |s|, clamped to [0.2, 2.5]. */
	float EffectiveExponent(const FSoilParams& Soil, float SlipRatioAbs);

	/** Chord contact patch length of a rigid wheel: sqrt(z * (2r - z)), z clamped to [0, 0.95r]. */
	float ChordPatchLength(float SinkageM, float RadiusM);

	/**
	 * Implicit Bekker solve for the rigid regime: b * k_eq * z^n * chord(z) = W.
	 * Newton from a closed-form small-z seed, bounds [1e-6, 0.95r], at most 8
	 * iterations, |dz| < 1e-5 m exit. Returns 0 for W <= 0.
	 */
	float SolveRigidSinkage(float NormalLoadN, float WidthM, float RadiusM, const FSoilParams& Soil, float SlipRatioAbs = 0.f);

	/**
	 * Wong's critical ground pressure p_gcr. A tire whose inflation + carcass
	 * pressure exceeds it behaves as a rigid wheel on this soil at this load.
	 */
	float CriticalGroundPressure(float NormalLoadN, float WidthM, float RadiusM, const FSoilParams& Soil, float SlipRatioAbs = 0.f);

	/** Explicit flexible-regime sinkage: z = (p / k_eq)^(1 / n_eff). */
	float FlexibleSinkage(float GroundPressurePa, float WidthM, const FSoilParams& Soil, float SlipRatioAbs = 0.f);

	/** Janosi saturation E(u) = 1 - (1 - e^-u)/u, series branch below u = 0.02 (float32 cancellation guard). Monotonic, E(0) = 0, E(inf) = 1. */
	float ShearSaturation(float U);

	/** Mohr-Coulomb shear budget: A * c + W * tan(phi). */
	float ShearBudget(float ContactAreaM2, float NormalLoadN, const FSoilParams& Soil);

	struct FShearForces
	{
		float LongitudinalN = 0.f; // signed: positive drives the wheel forward (sign of slip)
		float LateralN = 0.f;      // signed: opposes lateral sliding (minus sign of slip angle)
		float ResultantN = 0.f;    // sqrt(Fx^2 + Fy^2) <= budget by construction
	};

	/**
	 * Combined slip by resultant shear displacement (Wong; Ishigami et al.).
	 * Longitudinal and lateral shear share one soil budget; the friction
	 * ellipse emerges instead of being imposed.
	 */
	FShearForces CombinedShearForces(float ContactAreaM2, float NormalLoadN, float SlipRatio, float SlipAngleRad, float PatchLengthM, const FSoilParams& Soil);

	/** Compaction resistance R_c = b * k_eq * z^(n_eff+1) / (n_eff+1). */
	float CompactionResistance(float WidthM, float SinkageM, const FSoilParams& Soil, float SlipRatioAbs = 0.f);

	/** Rankine bulldozing resistance: 0.5*gamma*z^2*b*Kp + 2*c*z*b*sqrt(Kp), Kp = tan^2(pi/4 + phi/2). */
	float BulldozingResistance(float WidthM, float SinkageM, const FSoilParams& Soil);

	/**
	 * Regularized slip ratio (v_w - v_x) / max(|v_w|, |v_x|, Eps), clamped to
	 * [-1, 1]. Exactly 0 at rest (no NaN, no force chatter); equals the SAE
	 * definition for both driving and braking away from rest.
	 */
	float SlipRatio(float WheelSurfaceSpeedMS, float LongitudinalSpeedMS, float EpsMS = 0.1f);

	/** Regularized slip angle atan(v_y / max(|v_x|, Eps)). */
	float SlipAngle(float LateralSpeedMS, float LongitudinalSpeedMS, float EpsMS = 0.3f);

	/**
	 * PMDC hub motor: T = |cmd| * T_s * clamp(1 - omega*sign(cmd)/omega_0, 0, 1) * supply,
	 * signed by cmd. The lower clamp caps plugging (torque against rotation) at stall torque.
	 */
	float MotorTorque(float StallTorqueNm, float NoLoadSpeedRadS, float OmegaRadS, float Command01, float SupplyFraction = 1.f);

	/**
	 * Electrical draw: max(T*omega, 0)/eta + P_cu0 * (T/T_s)^2. Stall at full
	 * torque draws exactly P_cu0 - pure heat, which is what drains batteries
	 * when bogged in mud.
	 */
	float MotorElectricalPower(float DriveTorqueNm, float OmegaRadS, float StallTorqueNm, float Efficiency, float CopperLossAtStallW);

	/** Strut force max(0, k*C + c*Cdot), plus bump-stop spring past full travel. A tire never pulls the chassis down. */
	float SuspensionForce(float SpringNPerM, float DamperNSecPerM, float CompressionM, float CompressionRateMS, float TravelM, float BumpStopNPerM = 400000.f);

	/** Shear Control talent governor: 1 below TargetSlip, linear to 0 at FullCutSlip. Never applied to brakes. */
	float SlipGovernor(float SlipRatioAbs, float TargetSlip = 0.20f, float FullCutSlip = 0.30f);

	struct FWheelContactSolution
	{
		bool bRigid = true;
		float SinkageM = 0.f;         // z
		float PatchLengthM = 0.f;     // l
		float ContactAreaM2 = 0.f;    // A (W / p; kept unclamped for the shear budget)
		float GroundPressurePa = 0.f; // p
		float DeflectionM = 0.f;      // tire deflection delta (flexible regime; drives the visual bulge)
		float EffectiveRadiusM = 0.f; // r_eff for slip and reaction torque
	};

	/**
	 * Regime-aware contact solve (spec 4.1-4.4). PrevSlipRatioAbs feeds slip
	 * sinkage (one-substep lag breaks the z-s circularity); bPrevRigid feeds
	 * the 2 percent regime hysteresis that prevents flip-flapping at the
	 * p_gcr boundary. W <= 0 returns a zeroed, contact-free solution.
	 */
	FWheelContactSolution SolveWheelContact(float NormalLoadN, float WidthM, float RadiusM, float TirePressurePa, float CarcassPressurePa, const FSoilParams& Soil, float PrevSlipRatioAbs, bool bPrevRigid);
}
