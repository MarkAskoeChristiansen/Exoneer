// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "ExoneerTypes.generated.h"

class UItemDefinitionDataAsset;

/**
 * Material tier of a base piece. Higher tiers cost more, take more punishment,
 * carry more structural load, and shrug off storms better.
 */
UENUM(BlueprintType)
enum class EStructureTier : uint8
{
	Salvage,     // Crash-site scrap: cheap, weak, storm-vulnerable
	Alloy,       // Printed alloy: the standard mid-game tier
	Composite    // Reinforced composite: end-game
};

/** Machine state machine, replicated for UI/VFX. */
UENUM(BlueprintType)
enum class EMachineState : uint8
{
	Idle,
	Processing,
	OutputFull,
	LowPower
};

/** Lifecycle of anything built with the ghost-then-invest flow. */
UENUM(BlueprintType)
enum class EConstructionPhase : uint8
{
	Ghost,
	UnderConstruction,
	Complete
};

/** Why a build preview or placement request is invalid. */
UENUM(BlueprintType)
enum class EBuildPlacementError : uint8
{
	None,
	NoTarget,
	SocketOccupied,
	IncompatibleMount,
	NoSupport,
	BlockedByCollision,
	CellOccupied,
	NotAdjacent,
	OutOfReach,
	NotEnoughResources,
	InvalidDefinition,
	Unknown
};

/**
 * One item type + count. Used for authored data (build costs, recipes),
 * save games, and as the value type handed to inventory queries.
 * Runtime replicated storage uses FInventoryStack (InventoryComponent.h).
 */
USTRUCT(BlueprintType)
struct FInventoryEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Inventory")
	TSoftObjectPtr<UItemDefinitionDataAsset> Item;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Inventory")
	int32 Count = 0;
};

/**
 * One construction stage: the materials consumed across the stage and the
 * weld-work (weld-seconds) required to finish it. Pieces and vehicle blocks
 * declare an array of these; welding pulls materials from the builder's
 * inventory as progress passes proportional thresholds.
 */
USTRUCT(BlueprintType)
struct FConstructionCost
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Construction")
	TArray<FInventoryEntry> Materials;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Construction", meta = (ClampMin = "0.1"))
	float WeldWork = 5.f;
};
