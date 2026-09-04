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

	/** New-tire tread depth (mm). Wear spends this; 0 is scrap traction. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tire", meta = (ClampMin = "1"))
	float NewTreadDepthMm = 12.f;

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

	/**
	 * The OTHER interface: rubber on a hard surface (rock, a metal deck, the
	 * garage slab). There is no soil to shear, so TreadMobilisation has no
	 * referent - the hit material's own friction coefficient already IS the
	 * rubber-on-surface value, and multiplying it again by a soil-mobilisation
	 * fraction double counts. This factor is the tread pattern's share of that
	 * coefficient, and it runs the OPPOSITE way to TreadMobilisation: a smooth
	 * road tread lays down its whole contact patch (1.0) while deep lugs stand
	 * the carcass off the surface on a few bars of rubber and lose grip.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tire", meta = (ClampMin = "0.3", ClampMax = "1.2"))
	float HardSurfaceGrip = 1.f;

	/**
	 * Shear deformation modulus of the TREAD on a hard surface (m): how far
	 * the contact patch must slide before it develops its full friction. This
	 * is rubber deforming, a few millimetres, not a soil bed failing over
	 * centimetres - which is why a tire on tarmac reaches peak grip at a
	 * handful of degrees of slip angle instead of thirty.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tire", meta = (ClampMin = "0.0005", ClampMax = "0.02"))
	float TreadShearModulusM = 0.0025f;

	/** Lateral counterpart of TreadShearModulusM; a tire is slightly softer sideways. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tire", meta = (ClampMin = "0.0005", ClampMax = "0.02"))
	float TreadShearModulusLateralM = 0.003f;

	/**
	 * Contact speed below which the patch is solved as STUCK (static friction)
	 * rather than sliding (m/s). Coulomb friction is set-valued at zero
	 * sliding speed; this is the regularisation width, and both branches are
	 * bounded by the same Mohr-Coulomb budget, so it can never manufacture
	 * grip. Sliding is fully in charge above twice this speed.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tire", meta = (ClampMin = "0.005", ClampMax = "0.3"))
	float StickSpeedMS = 0.05f;

	// --- Suspension ---
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension") float SpringRateNPerM = 30000.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension") float DamperNSecPerM = 2100.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension") float RestLengthM = 0.32f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension") float TravelM = 0.22f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension") float BumpStopNPerM = 400000.f;
	/**
	 * Strut travel the bump stop absorbs past TravelM (m). The contact
	 * geometry tracks the ground over this range too, so the solved hub, the
	 * drawn hub and the bump-stop force all agree; past it the strut is
	 * genuinely out of travel.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension", meta = (ClampMin = "0.005", ClampMax = "0.2"))
	float BumpStopTravelM = 0.05f;
	/**
	 * How long a missed ground probe keeps the last known plane (s). The
	 * ground under a wheel does not vanish and reappear at 60 Hz; only the
	 * sampling does. The strut still decides contact - an out-of-reach plane
	 * reads airborne - so this restores nothing that geometry denies.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension", meta = (ClampMin = "0", ClampMax = "0.5"))
	float ContactGraceSeconds = 0.05f;

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

	// --- Thermal (lumped winding: losses in, Newton cooling to ambient out) ---
	// Sized against the two cases that matter. Bogged at full stall the motor
	// burns its whole copper loss, ~5 kW, which equilibrates near 200 C - well
	// past the trip, so digging in mud DOES cut the wheel out. Cruising at a
	// third of stall torque burns under 900 W and equilibrates near 50 C, so
	// ordinary driving never trips. Between those, derate is the warning.
	/** Torque starts fading here (C). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drive") float DerateOnsetTempC = 120.f;
	/** Cutout trips at or above this winding temperature (C): torque 0. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drive") float TripTempC = 150.f;
	/** Cutout clears only once the winding cools back to this (C) - hysteresis. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drive") float CutoutClearTempC = 110.f;
	/** Winding plus housing heat capacity (J/C): how long the motor tolerates an overload. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drive") float ThermalMassJPerC = 800.f;
	/** Newton cooling to ambient (W per C above ambient): sets the equilibrium temperature. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Drive") float CoolingWPerC = 28.f;

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
