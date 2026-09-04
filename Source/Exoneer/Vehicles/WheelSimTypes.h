// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Vehicles/ExoneerTerramechanics.h"

/**
 * Plain marshaling structs for the wheel physics, shared between the
 * game-thread UWheelModule (producer) and the construct's Chaos sim callback
 * (consumer, physics thread). NO UObject may be touched from the physics
 * thread, so everything a substep needs is copied here per frame.
 *
 * Units: SI unless the name says UU (Unreal units, cm). The only conversions
 * are at the body-state read and force-application edges.
 */
namespace ExoneerWheelSim
{
	/** Spec constants, copied from FVehicleWheelSpec at marshal time (SI). */
	struct FWheelSimConfig
	{
		float RadiusM = 0.35f;
		float WidthM = 0.22f;
		float SpringNPerM = 40000.f;
		float DamperNSecPerM = 3162.f;
		float RestLengthM = 0.30f;
		float TravelM = 0.18f;
		float BumpStopNPerM = 400000.f;
		float BumpStopTravelM = 0.05f;
		float WheelInertiaKgM2 = 1.f;
		float StallTorqueNm = 0.f;          // 0 for undriven wheels
		float NoLoadSpeedRadS = 40.f;
		float Efficiency = 0.85f;
		float CopperLossAtStallW = 2000.f;
		/** Controller electronics draw (W); part of the winding's heat input. */
		float ControllerIdleDrawW = 20.f;
		float MaxBrakeTorqueNm = 600.f;
		float RollingResistRigid = 0.008f;
		float RollingResistFlexible = 0.015f;
		float BearingDragNm = 1.5f;
		/** Fraction of soil shear strength the tread mobilises (grouser effect). */
		float TreadMobilisation = 0.8f;
		/** Share of the rubber-on-hard-surface friction the tread develops (no soil to shear). */
		float HardSurfaceGrip = 1.f;
		/** Tread shear deformation moduli on a hard surface (m) - rubber, not soil. */
		float TreadShearModulusM = 0.0025f;
		float TreadShearModulusLateralM = 0.003f;
		/** Contact speed below which the patch is solved as stuck (m/s). */
		float StickSpeedMS = 0.05f;
	};

	/** Per-frame commands and tire state (game-thread authored, sample-held over substeps). */
	struct FWheelSimCommand
	{
		float Throttle = 0.f;               // -1..1, already gated by bDriven
		float Brake = 0.f;                  // 0..1 service brake
		bool bParkingBrake = true;
		float SlipCap = 1.f;                // Shear Control talent lever
		float TirePressurePa = 180000.f;
		float CarcassPressurePa = 35000.f;
		float SupplyFraction = 1.f;
	};

	/** Per-frame ground cache + wheel frame, body-local so it tracks the body between substeps. */
	struct FWheelSimGround
	{
		bool bHasContact = false;
		FVector PlanePointUU = FVector::ZeroVector;   // world
		FVector PlaneNormal = FVector::UpVector;      // world unit
		ExoneerTerramechanics::FSoilParams Soil;
		FVector MountLocalUU = FVector::ZeroVector;   // suspension mount in BODY space
		FVector AxisLocal = -FVector::ZAxisVector;    // suspension axis (down) in BODY space, unit
		FVector ForwardLocal = FVector::XAxisVector;  // STEERED wheel forward in BODY space, unit
	};

	/** One wheel's full marshaled input. */
	struct FWheelSimInputItem
	{
		int32 BlockInstanceId = INDEX_NONE;
		FWheelSimConfig Config;
		FWheelSimCommand Command;
		FWheelSimGround Ground;
	};

	/** Persistent physics-side state (lives inside the sim callback across steps). */
	struct FWheelSimState
	{
		float OmegaRadS = 0.f;
		float PrevSlipAbs = 0.f;

		/**
		 * TWO radial drops, deliberately named apart so a later change cannot
		 * quietly merge them again. Both carry a one-substep lag and the same
		 * low-pass.
		 *
		 * SOLVED - what the contact mechanics ride on: tire deflection PLUS
		 * sinkage for a flexible tire, sinkage alone for a rigid one. The
		 * compaction, bulldozing and rut-drag terms are charged against
		 * exactly that geometry, so the solve must use it or the model
		 * contradicts itself.
		 *
		 * VISUAL - what the DRAWN hub rides on: tire deflection only. Sinkage
		 * is the wheel sitting in a rut that is not rendered, so folding it
		 * into the drawn hub buries the wheel in an undisturbed surface (on
		 * sand that is about 53 mm of it). Fold sinkage back in here once rut
		 * rendering exists.
		 */
		float PrevSolvedRadialDropM = 0.f;
		float PrevVisualRadialDropM = 0.f;

		bool bPrevRigid = true;
		/** Strut compression the suspension force was solved from (m). */
		float SolvedCompressionM = 0.f;
	};

	/** Per-frame telemetry marshaled back to the game thread. */
	struct FWheelSimTelemetry
	{
		int32 BlockInstanceId = INDEX_NONE;
		float OmegaRadS = 0.f;
		float SlipRatioAbs = 0.f;
		float SinkageM = 0.f;
		/**
		 * Strut compression for DRAWING and for the replicated quantum (m):
		 * tire squash only, no rut. See FWheelSimState for why the solved and
		 * the visual radial drop are two different quantities.
		 */
		float VisualCompressionM = 0.f;
		float NormalLoadN = 0.f;
		/** Tangential force actually developed in the patch this step (N). */
		float ShearForceN = 0.f;
		/** Resultant sliding speed of the patch over the ground (m/s). */
		float SlipSpeedMS = 0.f;
		float DriveTorqueNm = 0.f;
		float ElectricalPowerW = 0.f;   // this step's motor draw (mech/eta + copper loss)
		float LossPowerW = 0.f;         // heat into the winding: losses only, never shaft work
		bool bInContact = false;
	};

	/** Minimal body-state view read from the internal particle each substep (UU/rad). */
	struct FWheelBodyView
	{
		FTransform BodyTM;                    // world, UU
		FVector LinearVelocityUU = FVector::ZeroVector;   // at CoM, UU/s
		FVector AngularVelocityRad = FVector::ZeroVector; // world, rad/s
		FVector ComWorldUU = FVector::ZeroVector;
		/** Mass (kg) and the number of wheels sharing the body between them. */
		float MassKg = 1000.f;
		int32 WheelCount = 1;
		/** World rotation of the principal-inertia frame, and 1/I about those axes (kg*cm^2). */
		FQuat ComRotation = FQuat::Identity;
		FVector InvInertiaDiag = FVector::ZeroVector;

		FVector PointVelocityUU(const FVector& PointUU) const
		{
			return LinearVelocityUU + FVector::CrossProduct(AngularVelocityRad, PointUU - ComWorldUU);
		}

		/** World-space I^-1 applied to an angular impulse. */
		FVector ApplyInvInertia(const FVector& AngularUU) const
		{
			return ComRotation.RotateVector(ComRotation.UnrotateVector(AngularUU) * InvInertiaDiag);
		}

		/**
		 * Mass (kg) the body presents at a contact point along a unit direction:
		 *   1 / m_eff = 1/m + n . ((I^-1 (r x n)) x r)
		 * the standard contact effective mass. Lengths cancel between r and I,
		 * so it is computed directly in UU. The angular half matters: a sideways
		 * force half a metre below the centre of mass also ROLLS the body, so it
		 * cannot arrest the sliding as fast as the bare mass suggests. Using the
		 * bare mass instead lets a set of widely spaced wheels apply more angular
		 * impulse than the body's own inertia can absorb, which rings - exactly
		 * the class of overshoot that produced the creep in the first place.
		 */
		float EffectiveMassAt(const FVector& PointUU, const FVector& DirW) const
		{
			const FVector R = PointUU - ComWorldUU;
			const FVector AngularTerm = FVector::CrossProduct(
				ApplyInvInertia(FVector::CrossProduct(R, DirW)), R);
			const float InvEffective = 1.f / FMath::Max(MassKg, 1e-3f)
				+ FMath::Max(FVector::DotProduct(DirW, AngularTerm), 0.f);
			return 1.f / FMath::Max(InvEffective, 1e-9f);
		}

		/**
		 * That effective mass divided between the wheels that share the body.
		 * One wheel solving its own contact in isolation would arrest the whole
		 * body; N of them doing it at once would arrest it N times over.
		 */
		float ContactMassShareKg(const FVector& PointUU, const FVector& DirW) const
		{
			return EffectiveMassAt(PointUU, DirW) / FMath::Max((float)WheelCount, 1.f);
		}
	};

	/** Force to apply to the body this substep (UE units, world space). */
	struct FWheelSimForce
	{
		bool bApply = false;
		FVector ForceUE = FVector::ZeroVector;
		FVector LocationUU = FVector::ZeroVector;
	};

	/**
	 * One wheel, one substep: suspension, regime-aware Bekker contact,
	 * Janosi-Hanamoto combined shear, compaction + bulldozing resistance,
	 * stiction hold, motor/brake wheel-spin integration. Pure function -
	 * thread-agnostic, no engine state.
	 * Spec: docs/design/wheels/design-math-spec.md section 6.1 plus the
	 * critique's binding corrections (sim callback, stiction, motor model).
	 */
	FWheelSimForce StepWheel(float Dt, const FWheelSimInputItem& Input, const FWheelBodyView& Body,
		FWheelSimState& State, FWheelSimTelemetry& Telemetry);
}
