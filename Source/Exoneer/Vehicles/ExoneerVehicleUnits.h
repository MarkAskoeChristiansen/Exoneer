// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"

/**
 * Unit conversions between the SI terramechanics layer and UE units, plus the
 * dedicated wheel probe trace channel. The only code allowed to convert units
 * is the API edge that reads body state into the wheel solver and applies
 * forces back (docs/design/wheels/design-math-spec.md section 9). Everything
 * inside ExoneerTerramechanics stays pure SI.
 */
namespace ExoneerUnits
{
	/** Lengths and positions: SI -> UE multiply, UE -> SI divide. */
	constexpr float CmPerM = 100.f;

	/** N (kg*m/s^2) -> UE force (kg*cm/s^2). */
	constexpr float NewtonsToUEForce = 100.f;

	/** N*m -> UE torque (kg*cm^2/s^2), for AddTorqueInRadians. */
	constexpr float NmToUETorque = 10000.f;
}

/**
 * Wheel suspension probe channel, registered as "WheelProbe" in
 * Config/DefaultEngine.ini. Never probe on ECC_Visibility: ghost boxes block
 * it and would read as ground.
 */
#define ECC_WheelProbe ECC_GameTraceChannel1
