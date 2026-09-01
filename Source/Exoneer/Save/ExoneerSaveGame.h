// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "ExoneerTypes.h"
#include "ExoneerSaveGame.generated.h"

/**
 * One placed base piece (architecture or machine). The socket graph is NOT
 * saved: transforms are exact, so on load pieces re-link to the nearest free
 * compatible socket within 1 cm (spec section 12).
 */
USTRUCT(BlueprintType)
struct FSavedBasePiece
{
	GENERATED_BODY()

	/** Primary asset id name; resolves via FPrimaryAssetId("Piece", PieceId). */
	UPROPERTY() FName PieceId;
	UPROPERTY() FTransform WorldTransform;

	/** EConstructionPhase stored raw for save stability. */
	UPROPERTY() uint8 Phase = 0;
	UPROPERTY() int32 StageIndex = 0;
	UPROPERTY() float StageProgress01 = 0.f;
	UPROPERTY() float Health = 0.f;

	/** Machine internals; empty/zero for plain architecture. */
	UPROPERTY() TArray<FInventoryEntry> Inventory;
	UPROPERTY() float StoredEnergy = 0.f;

	/** Construction ledger, so deconstruction refunds survive a load. */
	UPROPERTY() TArray<FInventoryEntry> InvestedMaterials;
};

/** One base: its pieces in world space. */
USTRUCT(BlueprintType)
struct FSavedStructure
{
	GENERATED_BODY()

	UPROPERTY() TArray<FSavedBasePiece> Pieces;
};

/**
 * One vehicle block record. Origin/Orientation are grid placement inputs;
 * the rest restores the replicated record fields.
 */
USTRUCT(BlueprintType)
struct FSavedVehicleBlock
{
	GENERATED_BODY()

	/** Primary asset id name; resolves via FPrimaryAssetId("VehicleBlock", BlockId). */
	UPROPERTY() FName BlockId;
	UPROPERTY() FIntVector Origin = FIntVector::ZeroValue;
	UPROPERTY() uint8 Orientation = 0;
	UPROPERTY() int32 StageIndex = 0;
	UPROPERTY() float BuildProgress01 = 0.f;

	/** EConstructionPhase stored raw for save stability. */
	UPROPERTY() uint8 Phase = 0;
	UPROPERTY() float Health = 0.f;
	UPROPERTY() float StateScalar = 0.f;
};

/** One vehicle construct: its root transform and block records. */
USTRUCT(BlueprintType)
struct FSavedVehicle
{
	GENERATED_BODY()

	UPROPERTY() FTransform Transform;
	UPROPERTY() TArray<FSavedVehicleBlock> Blocks;
};

UCLASS(BlueprintType)
class EXONEER_API UExoneerSaveGame : public USaveGame
{
	GENERATED_BODY()
public:
	UPROPERTY() FString SlotName = TEXT("ExoneerDefault");

	// --- Player ---
	UPROPERTY() FTransform PlayerTransform;
	UPROPERTY() TArray<FInventoryEntry> PlayerInventory;

	UPROPERTY() float PlayerHealth = 100.f;
	UPROPERTY() float PlayerOxygen = 100.f;
	UPROPERTY() float PlayerSuitPower = 100.f;
	UPROPERTY() float PlayerBodyTempC = 36.6f;

	// --- Environment ---
	UPROPERTY() float TimeOfDay = 0.25f;

	// --- World construction state ---
	UPROPERTY() TArray<FSavedStructure> Structures;
	UPROPERTY() TArray<FSavedVehicle> Vehicles;
};
