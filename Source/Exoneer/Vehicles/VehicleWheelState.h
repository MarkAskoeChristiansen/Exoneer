// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Net/Serialization/FastArraySerializer.h"
#include "VehicleWheelState.generated.h"

/**
 * Quantized per-wheel state, replicated as a SIDE fast array on the construct
 * - deliberately separate from FVehicleBlockRecord, whose every replicated
 * change triggers a full client visual rebuild. These callbacks are EMPTY on
 * purpose: clients POLL the array from Tick for wheel visual poses and the
 * drivetrain summary; nothing here may ever call MarkVisualsDirty.
 *
 * The server writes under deadbands (steer 0.01 rad, omega 0.25 rad/s, one
 * quantum on the rest); slip/sinkage/pressure are dashboard data, not
 * animation data, and are additionally rate-limited at the writer.
 */
USTRUCT()
struct FVehicleWheelStateItem : public FFastArraySerializerItem
{
	GENERATED_BODY()

	UPROPERTY() int32 BlockInstanceId = INDEX_NONE;
	UPROPERTY() int16 SteerAngleQ = 0;    // rad * 1000
	UPROPERTY() int16 OmegaQ = 0;         // rad/s * 64
	UPROPERTY() uint8 SlipQ = 0;          // |s| 0..1 -> 0..255
	UPROPERTY() uint8 SinkageQ = 0;       // z in cm, 0..255
	UPROPERTY() uint8 CompressionQ = 0;   // compression / travel -> 0..255 (suspension visual)
	UPROPERTY() uint8 TirePressureQ = 0;  // kPa / 2

	float GetSteerAngleRad() const { return SteerAngleQ / 1000.f; }
	float GetOmegaRadS() const { return OmegaQ / 64.f; }
	float GetSlipRatioAbs() const { return SlipQ / 255.f; }
	float GetSinkageM() const { return SinkageQ / 100.f; }
	float GetCompression01() const { return CompressionQ / 255.f; }
	float GetTirePressureKPa() const { return TirePressureQ * 2.f; }

	void PostReplicatedAdd(const struct FVehicleWheelStateList&) {}
	void PostReplicatedChange(const struct FVehicleWheelStateList&) {}
	void PreReplicatedRemove(const struct FVehicleWheelStateList&) {}
};

USTRUCT()
struct FVehicleWheelStateList : public FFastArraySerializer
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FVehicleWheelStateItem> Items;

	FVehicleWheelStateItem* FindByBlockId(int32 BlockInstanceId)
	{
		return Items.FindByPredicate([BlockInstanceId](const FVehicleWheelStateItem& Item)
		{
			return Item.BlockInstanceId == BlockInstanceId;
		});
	}

	const FVehicleWheelStateItem* FindByBlockId(int32 BlockInstanceId) const
	{
		return Items.FindByPredicate([BlockInstanceId](const FVehicleWheelStateItem& Item)
		{
			return Item.BlockInstanceId == BlockInstanceId;
		});
	}

	bool NetDeltaSerialize(FNetDeltaSerializeInfo& DeltaParms)
	{
		return FFastArraySerializer::FastArrayDeltaSerialize<FVehicleWheelStateItem, FVehicleWheelStateList>(Items, DeltaParms, *this);
	}
};

template<>
struct TStructOpsTypeTraits<FVehicleWheelStateList> : public TStructOpsTypeTraitsBase2<FVehicleWheelStateList>
{
	enum { WithNetDeltaSerializer = true };
};

/**
 * "No winding reading" sentinel (C): below absolute zero, so it can never
 * collide with a real temperature on a cold planet the way a 0 or -1 would.
 */
inline constexpr float NoWindingReadingC = -1000.f;

/** Aggregate drivetrain readout for instrumentation (interim visor HUD now, diegetic dashboard later). */
USTRUCT(BlueprintType)
struct FVehicleDrivetrainSummary
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float WorstSlipRatio = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float MaxSinkageM = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float MinTirePressureKPa = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float SpeedMS = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") int32 WheelCount = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") int32 WheelsInContact = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") bool bParkingBrake = false;
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") bool bCanDrive = false;
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") bool bCanFly = false;
	/** Total installed gyro torque (N*m). 0 = no attitude authority at all. */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float GyroTorqueNm = 0.f;
	/**
	 * Worst-axis fraction of the reaction-wheel momentum envelope in use,
	 * 0..1. At 1.0 that axis makes no more torque in the winding direction
	 * until the momentum is dumped against an external torque, so the pilot
	 * needs to see it climbing before it arrives.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float GyroSaturation01 = 0.f;
	/**
	 * Which body axis carries that worst store: 0 roll, 1 pitch, 2 yaw. A
	 * percentage with no axis name told the pilot something was wrong and
	 * nothing about what to stop doing.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") uint8 GyroWorstAxis = 0;
	/** Seat bank and pitch (deg). A craft that holds its attitude must show it. */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float BankDeg = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float PitchDeg = 0.f;
	/**
	 * True only when a propellant tank is actually installed. Without it the
	 * FUEL line pulsed red from the first second of every flight on a craft
	 * that has no tank and needs none - in the same colour as the two lines
	 * that matter, which is how a pilot learns to ignore all three.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") bool bHasFuelCapacity = false;
	/**
	 * True while the lift governor is FROZEN because the craft is banked past
	 * the angle at which the reserved ceiling can hold weight. It is not
	 * holding altitude; it is sinking with the valve stuck where it was.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") bool bLiftGovernorPinned = false;
	/** True while the governor is flying the descend key's bounded descent rate. */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") bool bLiftDescending = false;
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float MinTreadDepthMm = -1.f;
	/** Hottest Complete wheel winding (C). NoWindingReadingC = no wheel motor installed. */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float MaxWindingTempC = NoWindingReadingC;
	/** Any wheel past its derate onset: torque capacity is already fading. */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") bool bAnyWindingDerating = false;
	/** Any wheel tripped its over-temp cutout: that motor makes no torque until it cools. */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") bool bAnyThermalCutout = false;
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float StoredFuelKg = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float AscentTwr = 0.f;
	/**
	 * Lift valve setting, 0..1. The primary flight control had no readout at
	 * all, on a visor carrying seventeen other numbers.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float LiftFraction01 = 0.f;
	/** Signed vertical speed (m/s). SpeedMS is a magnitude and hides a pure climb. */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float VerticalSpeedMS = 0.f;
	/**
	 * True while the lift GOVERNOR is holding the valve rather than a finger.
	 * A valve that is open because a computer decided so has to say which it
	 * is, or the pilot cannot tell hold from a latch.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") bool bLiftGovernorActive = false;
	/**
	 * Worst-axis standing moment the thrust group could not cancel (N*m). On a
	 * balanced build it is zero. Anything else is being paid for out of rotor
	 * momentum at this many N*m*s per second, so the visor turns it and
	 * GyroMomentumCapacityNms into the seconds the axis has left.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float UntrimmedStandingMomentNm = 0.f;
	/** Which body axis carries that residual: 0 roll, 1 pitch, 2 yaw. */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") uint8 UntrimmedWorstAxis = 0;
	/**
	 * Standing LATERAL force the pilot's own throttles make, along the seat's
	 * right axis (N). Flight has no lateral thrust command, and the trim path
	 * nulls only the net TRIM force, so this is a push nobody asked for and
	 * nothing can answer - and it is invisible in every other readout. A build
	 * whose lift nozzles are all toed the same way drifts at 1904 N with an
	 * empty momentum store.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float StandingSideForceN = 0.f;
	/**
	 * True while the craft's lift has no upward component left at all: the
	 * governor has SHUT the valve because opening it would push the craft at
	 * the ground. Distinct from bLiftGovernorPinned, and the pilot's way out is
	 * different - roll back level, do not add throttle.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") bool bLiftInverted = false;
	/** Installed rotor momentum capacity per axis (N*m*s). 0 = no triad installed. */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float GyroMomentumCapacityNms = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float StoredEnergyWs = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float EnergyCapacityWs = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float LastLandingSpeedMS = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle") float HullHealth01 = 1.f;
};
