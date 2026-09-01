// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "PilotInput.generated.h"

/**
 * How a construct interprets pilot input.
 * Flight: move = thrust intents, look = gyro torque (the v1 thruster scheme).
 * Ground: W/S = drive throttle, A/D = steer, look = free camera; the gyro is
 * gated to GroundModeGyroFraction so a rover cannot attitude-thrust mid-air.
 */
UENUM(BlueprintType)
enum class EPilotControlMode : uint8
{
	Flight,
	Ground
};

namespace EPilotHeldFlags
{
	enum Type : uint8
	{
		Handbrake = 1 << 0,
	};
}

/**
 * One pilot intent packet, sent unreliably at ~20 Hz from the pilot's pawn to
 * the server, which HOLDS the last packet until the next one or a timeout
 * (replacing the old per-tick decay that sagged 40 percent between sends).
 *
 * Axes are per-send-window samples in [-1, 1]; held states ride as flags so a
 * hold can never be lost to packet timing; the mode toggle is a 2-bit rolling
 * counter so discrete presses between sends latch losslessly.
 */
USTRUCT(BlueprintType)
struct FPilotInput
{
	GENERATED_BODY()

	/** Flight-mode thrust intent in the cockpit frame (X fwd, Y right, Z up), each axis [-1, 1]. */
	UPROPERTY(BlueprintReadWrite, Category = "Pilot")
	FVector Move = FVector::ZeroVector;

	/** Flight-mode gyro intent (pitch, yaw, roll), each axis [-1, 1]. */
	UPROPERTY(BlueprintReadWrite, Category = "Pilot")
	FVector Rotate = FVector::ZeroVector;

	/** Ground-mode drive throttle [-1, 1]. */
	UPROPERTY(BlueprintReadWrite, Category = "Pilot")
	float Throttle = 0.f;

	/** Ground-mode steer [-1, 1], positive = right. */
	UPROPERTY(BlueprintReadWrite, Category = "Pilot")
	float Steer = 0.f;

	/** Service brake [0, 1]. */
	UPROPERTY(BlueprintReadWrite, Category = "Pilot")
	float Brake = 0.f;

	/** EPilotHeldFlags bits, sampled at send time (never zeroed between sends). */
	UPROPERTY(BlueprintReadWrite, Category = "Pilot")
	uint8 HeldFlags = 0;

	/** Rolling 2-bit counter of control-mode toggle presses. */
	UPROPERTY(BlueprintReadWrite, Category = "Pilot")
	uint8 ModeToggleCount = 0;

	bool ContainsNaN() const
	{
		return Move.ContainsNaN() || Rotate.ContainsNaN()
			|| !FMath::IsFinite(Throttle) || !FMath::IsFinite(Steer) || !FMath::IsFinite(Brake);
	}

	/** Clamp every axis to its documented range (server-side sanitization). */
	void Sanitize()
	{
		Move = Move.BoundToCube(1.f);
		Rotate = Rotate.BoundToCube(1.f);
		Throttle = FMath::Clamp(Throttle, -1.f, 1.f);
		Steer = FMath::Clamp(Steer, -1.f, 1.f);
		Brake = FMath::Clamp(Brake, 0.f, 1.f);
		ModeToggleCount &= 0x3;
	}

	/** Zero the axes but keep held flags and the toggle counter (timeout behavior). */
	void ZeroAxes()
	{
		Move = FVector::ZeroVector;
		Rotate = FVector::ZeroVector;
		Throttle = 0.f;
		Steer = 0.f;
		Brake = 0.f;
	}

	bool NetSerialize(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess);
};

template<>
struct TStructOpsTypeTraits<FPilotInput> : public TStructOpsTypeTraitsBase2<FPilotInput>
{
	enum { WithNetSerializer = true };
};
