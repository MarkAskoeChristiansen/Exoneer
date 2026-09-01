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
		float WheelInertiaKgM2 = 1.f;
		float StallTorqueNm = 0.f;          // 0 for undriven wheels
		float NoLoadSpeedRadS = 40.f;
		float Efficiency = 0.85f;
		float CopperLossAtStallW = 2000.f;
		float MaxBrakeTorqueNm = 600.f;
		float RollingResistRigid = 0.008f;
		float RollingResistFlexible = 0.015f;
		float BearingDragNm = 1.5f;
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
		float PrevRadialDropM = 0.f;   // z (rigid) or tire deflection (flexible), one-substep lag
		bool bPrevRigid = true;
		float CompressionM = 0.f;
	};

	/** Per-frame telemetry marshaled back to the game thread. */
	struct FWheelSimTelemetry
	{
		int32 BlockInstanceId = INDEX_NONE;
		float OmegaRadS = 0.f;
		float SlipRatioAbs = 0.f;
		float SinkageM = 0.f;
		float CompressionM = 0.f;
		float NormalLoadN = 0.f;
		float DriveTorqueNm = 0.f;
		float ElectricalPowerW = 0.f;   // this step's motor draw (mech/eta + copper loss)
		bool bInContact = false;
	};

	/** Minimal body-state view read from the internal particle each substep (UU/rad). */
	struct FWheelBodyView
	{
		FTransform BodyTM;                    // world, UU
		FVector LinearVelocityUU = FVector::ZeroVector;   // at CoM, UU/s
		FVector AngularVelocityRad = FVector::ZeroVector; // world, rad/s
		FVector ComWorldUU = FVector::ZeroVector;

		FVector PointVelocityUU(const FVector& PointUU) const
		{
			return LinearVelocityUU + FVector::CrossProduct(AngularVelocityRad, PointUU - ComWorldUU);
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
