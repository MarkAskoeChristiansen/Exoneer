// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Styling/SlateBrush.h"
#include "ItemDefinitionDataAsset.generated.h"

/**
 * Category used to filter items in inventories and crafting UIs.
 */
UENUM(BlueprintType)
enum class EExoneerItemCategory : uint8
{
	Raw,           // Stone, ice, ore
	Refined,       // Iron ingot, silicon wafer
	Component,     // Computer board, motor, plate
	Block,         // Building blocks (placed via build tool)
	Tool,
	Consumable,
	Fuel,
	Misc
};

/**
 * Data-driven description of any inventory item in Exoneer.
 * Create instances as UItemDefinitionDataAsset assets under
 * /Content/Exoneer/Data/Items/.
 */
UCLASS(BlueprintType)
class EXONEER_API UItemDefinitionDataAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Stable string identifier ("stone", "iron_ingot", "thruster_small", ...). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AssetRegistrySearchable, Category = "Item")
	FName ItemId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item", meta = (MultiLine = "true"))
	FText Description;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item")
	EExoneerItemCategory Category = EExoneerItemCategory::Misc;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item")
	int32 MaxStack = 100;

	/** Per-unit mass, kg. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item")
	float Mass = 1.f;

	/** Per-unit volume, used for cargo capacity calculations. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item")
	float Volume = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item")
	TSoftObjectPtr<UTexture2D> Icon;

	/** If this item, when wielded or placed, corresponds to a block. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Item")
	TSoftObjectPtr<class UBlockDefinitionDataAsset> AssociatedBlock;

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId(TEXT("Item"), ItemId);
	}
};
