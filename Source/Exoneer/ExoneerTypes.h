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

/**
 * Causal condition of one installed part (GAME-SCOPE §10). Not a 0–1
 * durability bar. Fields that do not apply to a part class stay at the
 * N/A sentinel (tread < 0).
 */
USTRUCT(BlueprintType)
struct FPartCondition
{
	GENERATED_BODY()

	/** Tire tread (mm). < 0 = this part is not a tire. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Condition")
	float TreadDepthMm = -1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Condition")
	float CarcassTempC = 20.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Condition")
	float InflationKPa = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Condition")
	float WindingTempC = 20.f;

	/**
	 * Over-temp cutout latch (motors): torque capacity 0 until the winding
	 * cools back to the spec's clear temperature. The one stored terminal
	 * state GAME-SCOPE 10 allows, because hysteresis cannot be derived from
	 * the reading alone. Recovers by cooling; a scrap winding is post-alpha.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Condition")
	uint8 bThermalCutout : 1 = false;

	/** 0 = nominal capacity, 1 = dead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Condition")
	float CapacityFade01 = 0.f;

	/** Dust / opacity on exposed faces; production *= (1 - opacity). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Condition")
	float SurfaceOpacity01 = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Condition")
	float LeakRateLps = 0.f;

	/**
	 * How many times this part was patched. Saved and replicated with the
	 * struct. A suit seal that has been patched three times is worn out:
	 * further kits are refused and only a fabricated spare resets it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Condition")
	uint8 PatchCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Condition")
	float DeflectionMm = 0.f;

	bool HasTire() const { return TreadDepthMm >= 0.f; }
};

UENUM(BlueprintType)
enum class EProjectState : uint8
{
	Available,
	Active,
	Succeeded,
	Failed,
	Abandoned
};

UENUM(BlueprintType)
enum class EProjectCriterionType : uint8
{
	PowerReserveHours,
	OxygenReserveHours,
	CommsHops,
	StormSurvived,
	DishComplete,
	PadPowered,
	FuelMassKg,
	AscentTwr
};

USTRUCT(BlueprintType)
struct FProjectCriterion
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Project")
	EProjectCriterionType Type = EProjectCriterionType::PowerReserveHours;

	/** SI / hours / hop-count depending on Type. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Project")
	float Target = 1.f;
};

USTRUCT(BlueprintType)
struct FProjectRuntime
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Project")
	FName ProjectId;

	UPROPERTY(BlueprintReadOnly, Category = "Project")
	EProjectState State = EProjectState::Available;

	UPROPERTY(BlueprintReadOnly, Category = "Project")
	float StartedTimeOfDay01 = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Project")
	int32 StartedSol = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Project")
	int32 SolsActive = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Project")
	bool bSawStorm = false;

	UPROPERTY(BlueprintReadOnly, Category = "Project")
	FString LastFailReason;
};

USTRUCT(BlueprintType)
struct FOrbitalKnowledge
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Project")
	bool bHasHandshake = false;

	/** Next launch window as TimeOfDay01. */
	UPROPERTY(BlueprintReadOnly, Category = "Project")
	float NextWindowTimeOfDay01 = 0.5f;

	UPROPERTY(BlueprintReadOnly, Category = "Project")
	float PadSlopeLimitDeg = 8.f;
};
