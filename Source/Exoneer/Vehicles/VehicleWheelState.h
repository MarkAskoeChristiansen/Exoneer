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
};
