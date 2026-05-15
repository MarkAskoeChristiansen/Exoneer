// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "BlockDefinitionDataAsset.generated.h"

class UItemDefinitionDataAsset;
class ABuildableBlock;

UENUM(BlueprintType)
enum class EExoneerBlockCategory : uint8
{
	Structure,        // Foundation, floor, wall, window
	Armor,            // Light/heavy armor
	Door,             // Door, airlock, ladder
	Power,            // Solar, wind, battery, generator
	Storage,          // Cargo, oxygen tank, fuel tank
	Production,       // Refinery, fabricator, oxygen generator, food printer
	Logistics,        // Conveyor tube
	ShipModule,       // Cockpit, seat, gyro, thruster, landing gear, dock
	Survival,         // Survival kit, medical station
	Utility,          // Drill, spotlight, antenna, beacon, control console
	Decorative
};

UENUM(BlueprintType)
enum class EExoneerBlockSizeClass : uint8
{
	Small UMETA(DisplayName = "Small Grid (50cm)"),
	Large UMETA(DisplayName = "Large Grid (250cm)")
};

USTRUCT(BlueprintType)
struct FBlockBuildCost
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Cost")
	TSoftObjectPtr<UItemDefinitionDataAsset> Item;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Cost")
	int32 Count = 1;
};

USTRUCT(BlueprintType)
struct FBlockConnectionPoint
{
	GENERATED_BODY()

	/** Local-space offset of the connection in grid cells. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Connection")
	FIntVector LocalCell = FIntVector::ZeroValue;

	/** Outward-facing direction (unit axis vector in grid space). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Connection")
	FIntVector Direction = FIntVector(1, 0, 0);

	/** Tag classifying the connection (Power, Conveyor, Oxygen, ...). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Connection")
	FName ConnectionTag;
};

/**
 * Definition of a single buildable block type.
 * Create one asset per block (Floor, Wall, SolarPanel, Refinery, Thruster, ...)
 * under /Content/Exoneer/Data/Blocks/.
 */
UCLASS(BlueprintType)
class EXONEER_API UBlockDefinitionDataAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AssetRegistrySearchable, Category = "Block")
	FName BlockId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block", meta = (MultiLine = "true"))
	FText Description;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block")
	EExoneerBlockCategory Category = EExoneerBlockCategory::Structure;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block")
	EExoneerBlockSizeClass SizeClass = EExoneerBlockSizeClass::Small;

	/** Number of grid cells occupied along X, Y, Z. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block")
	FIntVector GridSize = FIntVector(1, 1, 1);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block")
	float Mass = 100.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block")
	float MaxHealth = 200.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block|Build")
	TArray<FBlockBuildCost> BuildCost;

	/** Watts consumed (negative) or produced (positive) when active. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block|Power")
	float PowerDelta = 0.f;

	/** Oxygen produced per second by this block, if relevant. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block|Oxygen")
	float OxygenProduction = 0.f;

	/** Litres of fuel storage / capacity. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block|Fuel")
	float FuelCapacity = 0.f;

	/** Inventory volume in cargo units. 0 means no inventory. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block|Storage")
	float InventoryCapacity = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block|Connection")
	TArray<FBlockConnectionPoint> ConnectionPoints;

	/** Block must be placed on top of another block (true) or can free-float (false). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block|Placement")
	bool bRequiresSupport = true;

	/** Allowed on base grids. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block|Placement")
	bool bAllowedOnBase = true;

	/** Allowed on ship/vehicle grids. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block|Placement")
	bool bAllowedOnVehicle = false;

	/** Actor class spawned when this block is built. Usually a BP child of ABuildableBlock. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block|Visual")
	TSubclassOf<ABuildableBlock> BlockActorClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block|Visual")
	TSoftObjectPtr<UStaticMesh> PreviewMesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block|Visual")
	TSoftObjectPtr<UTexture2D> Icon;

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId(TEXT("Block"), BlockId);
	}
};
