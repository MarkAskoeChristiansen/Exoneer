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

	/**
	 * Tread mobilisation factor: the fraction of the soil's own shear strength
	 * the contact can develop (Bekker's grouser effect, Wong's treatment of
	 * lugged wheels). A smooth tire shears RUBBER against soil, which is
	 * weaker than the soil's internal strength, so it sits below 1. A deeply
	 * lugged tire shears SOIL against soil and adds lug bearing, so it can
	 * exceed 1. This is the physical reason a mud tire grips mud: it is not a
	 * bonus, it is a different interface.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tire", meta = (ClampMin = "0.3", ClampMax = "1.4"))
	float TreadMobilisation = 0.8f;

	// --- Suspension ---
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension") float SpringRateNPerM = 30000.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension") float DamperNSecPerM = 2100.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension") float RestLengthM = 0.32f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension") float TravelM = 0.22f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension") float BumpStopNPerM = 400000.f;

	// --- Drivetrain (ideal PMDC hub motor: T = T_s * (1 - w/w_0)) ---
	// Sized so a ~1.6 t rover cruises ~11 m/s on clay and ~17 m/s on hard
	// ground: the equilibrium speed is w_0 * (1 - T_needed/T_s) * r, and the
	// first 250 Nm motors equilibrated at a crawl against soft-soil drag.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drive") bool bDriven = true;
	/**
	 * 300 N*m per hub is deliberately modest: six of them clear a 10 degree
	 * grade but not 20, so the ramp stays a progression gate for a later
	 * higher-torque hub motor rather than something the starter rover walks up.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drive") float MaxMotorTorqueNm = 300.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drive") float NoLoadSpeedRadS = 52.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drive", meta = (ClampMin = "0.1", ClampMax = "1"))
	float DrivetrainEfficiency = 0.85f;
	/** Resistive (I^2 R) loss at full stall torque - what a bogged motor burns as pure heat. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drive") float CopperLossAtStallW = 5000.f;
	/** Constant motor-controller electronics draw while the block is Complete. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drive") float ControllerIdleDrawW = 20.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drive") float MaxBrakeTorqueNm = 600.f;

	// --- Steering ---
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Steer") bool bSteerable = false;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Steer") float MaxSteerAngleDeg = 35.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Steer") float SteerRateDegPerS = 90.f;

	// --- Tire (ground pressure model: p = inflation + carcass) ---
	// Operating pressure is a PROPERTY OF THE TIRE, not a player control: you
	// choose the right wheel for the terrain, you do not fiddle with a valve
	// while driving. Wong's critical ground pressure on the authored soils is
	// roughly 65-130 kPa at rover wheel loads, so a 220 kPa road tire is rigid
	// everywhere and digs in, while a 45 kPa balloon tire floats. That
	// threshold is what makes terrain-specific wheels matter.
	// (The Heavy Off-Road talent later unlocks an in-cockpit CTIS valve that
	// can run a tire below its nominal, down to MinTirePressureKPa.)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tire") float NominalTirePressureKPa = 220.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tire") float MinTirePressureKPa = 60.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tire") float MaxTirePressureKPa = 300.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tire") float CarcassStiffnessKPa = 15.f;

	// --- Rolling resistance (internal: tire hysteresis + hub) ---
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel") float RollingResistRigid = 0.008f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel") float RollingResistFlexible = 0.015f;
	/** Constant bearing/seal drag torque at the hub, always opposing spin. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel") float BearingDragNm = 1.5f;

	/** kg*m^2. 0 = derive as 0.6 * block Mass * RadiusM^2 (tire mass sits at the rim). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel") float WheelInertiaOverrideKgM2 = 0.f;
};
