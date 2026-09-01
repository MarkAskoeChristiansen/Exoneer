// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "VehicleWheelSpec.generated.h"

/**
 * Authored physical description of one wheel block, SI units in the names.
 * Consumed by UWheelModule; the terramechanics math is in
 * Vehicles/ExoneerTerramechanics.h (spec: docs/design/wheels/design-math-spec.md).
 *
 * Suspension stability bounds at 120 Hz substeps (spec 6.3): for a corner
 * mass share m, require SpringRateNPerM <= 0.25*m/dt^2 and
 * DamperNSecPerM <= m/dt; the defaults hold a 1000 kg, 4-wheel rover with a
 * factor >= 5 margin. Validated at module init with a log warning so a bad
 * authored value is a content error, not a physics explosion.
 */
USTRUCT(BlueprintType)
struct FVehicleWheelSpec
{
	GENERATED_BODY()

	// --- Geometry (m) ---
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel", meta = (ClampMin = "0.05"))
	float RadiusM = 0.35f;

	/** Bekker 'b'. Keep bulge clearance inside the block cells. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel", meta = (ClampMin = "0.02"))
	float WidthM = 0.22f;

	// --- Suspension ---
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension") float SpringRateNPerM = 40000.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension") float DamperNSecPerM = 3162.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension") float RestLengthM = 0.30f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension") float TravelM = 0.18f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension") float BumpStopNPerM = 400000.f;

	// --- Drivetrain (ideal PMDC hub motor: T = T_s * (1 - w/w_0)) ---
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drive") bool bDriven = true;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drive") float MaxMotorTorqueNm = 250.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drive") float NoLoadSpeedRadS = 40.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drive", meta = (ClampMin = "0.1", ClampMax = "1"))
	float DrivetrainEfficiency = 0.85f;
	/** Resistive (I^2 R) loss at full stall torque - what a bogged motor burns as pure heat. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drive") float CopperLossAtStallW = 2000.f;
	/** Constant motor-controller electronics draw while the block is Complete. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drive") float ControllerIdleDrawW = 20.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drive") float MaxBrakeTorqueNm = 600.f;

	// --- Steering ---
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Steer") bool bSteerable = false;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Steer") float MaxSteerAngleDeg = 35.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Steer") float SteerRateDegPerS = 90.f;

	// --- Tire (ground pressure model: p = inflation + carcass; CTIS moves inflation only) ---
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tire") float NominalTirePressureKPa = 180.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tire") float MinTirePressureKPa = 60.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tire") float MaxTirePressureKPa = 300.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tire") float CarcassStiffnessKPa = 35.f;
	/** Physical valve rate for the CTIS hold-to-pump keys. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tire") float CtisRateKPaPerS = 20.f;

	// --- Rolling resistance (internal: tire hysteresis + hub) ---
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel") float RollingResistRigid = 0.008f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel") float RollingResistFlexible = 0.015f;
	/** Constant bearing/seal drag torque at the hub, always opposing spin. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel") float BearingDragNm = 1.5f;

	/** kg*m^2. 0 = derive as 0.6 * block Mass * RadiusM^2 (tire mass sits at the rim). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel") float WheelInertiaOverrideKgM2 = 0.f;
};
