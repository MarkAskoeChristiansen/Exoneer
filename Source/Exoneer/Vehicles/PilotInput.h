// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "PilotInput.generated.h"

/**
 * How a construct interprets pilot input.
 *
 * Flight: W/S = forward and back thrust, A/D = roll rate, mouse = pitch and
 * yaw RATE, lift key = climb while HELD, descend key = close the valve while
 * HELD, and neither held airborne = the lift governor holds altitude.
 *
 * Every attitude demand returns to zero when the key is released and the triad
 * nulls whatever body rate is left, so letting go stops the craft rotating.
 * Lift returns to a HOVER rather than to zero, which is the one thing a craft
 * with a 1.2 thrust-to-weight ratio needs and a bare valve cannot give: the
 * alternatives were climbing at 0.3 g or falling at 1 g with nothing between
 * them. The governor is not a latch - the descend key shuts it, so does
 * touching down, an input timeout, leaving the seat, or switching to Ground -
 * and it carries no integrator, so its whole state is a vertical speed the
 * pilot can read. There is still no attitude hold and no latched collective.
 *
 * Ground: W/S = drive throttle, A/D = steer, look = free camera, and the lift
 * valve is shut. The triad still rate-nulls with a pilot aboard, in both modes
 * and loaded wheels or not: a reaction wheel really does resist rotation while
 * the hull sits on its tyres, it is the pilot's instinctive "make it stop",
 * and the wheel-contact bleed keeps whatever it stores from being permanent.
 * So the mode toggle is never a tumble switch at altitude, and it never takes
 * damping away on the ground either.
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
		/** GROUND only: hold the parking brake. Suppressed in Flight. */
		Handbrake = 1 << 0,
		/**
		 * FLIGHT only: close the lift valve. It rides as a HELD FLAG, not an
		 * axis, for two reasons - a hold can never be lost to packet timing,
		 * and ZeroAxes deliberately does not clear it, so an input timeout
		 * leaves the craft descending rather than hovering on a dead link.
		 */
		Descend = 1 << 1,
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

	/**
	 * Flight-mode thrust intent in the cockpit frame. X/Y are a horizontal
	 * direction and magnitude; Z is the CLIMB level: 1 while the lift key is
	 * held, 0 the moment it is released.
	 *
	 * Zero does not mean hold. The server decides the valve target from three
	 * inputs - this level, the Descend held flag, and whether the craft is
	 * airborne under a live pilot - and ZeroAxes plus the staleness check below
	 * is what makes a timeout close the valve rather than hover on a dead link.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "Pilot")
	FVector Move = FVector::ZeroVector;

	/** Flight-mode attitude RATE command (pitch, yaw, roll), each axis [-1, 1] of the craft's rate limit. */
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

	/**
	 * Zero the axes but keep held flags and the toggle counter (timeout
	 * behavior). Move.Z is a LEVEL, so this drops the climb demand; the server
	 * additionally treats a stale packet as no pilot for the lift governor, so
	 * the valve closes instead of holding altitude on a dead link. Descend is a
	 * held flag and survives on purpose - failing toward the ground is the
	 * honest response to input that has stopped arriving.
	 */
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
