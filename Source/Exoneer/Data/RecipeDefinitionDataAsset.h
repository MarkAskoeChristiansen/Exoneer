// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RecipeDefinitionDataAsset.generated.h"

class UItemDefinitionDataAsset;

UENUM(BlueprintType)
enum class EExoneerRecipeStation : uint8
{
	PlayerHand,
	Fabricator,
	Refinery,
	OxygenGenerator,
	FoodPrinter
};

USTRUCT(BlueprintType)
struct FRecipeIngredient
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recipe")
	TSoftObjectPtr<UItemDefinitionDataAsset> Item;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recipe")
	int32 Count = 1;
};

/**
 * A single crafting recipe. Create assets under /Content/Exoneer/Data/Recipes/.
 */
UCLASS(BlueprintType)
class EXONEER_API URecipeDefinitionDataAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AssetRegistrySearchable, Category = "Recipe")
	FName RecipeId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recipe")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recipe")
	EExoneerRecipeStation Station = EExoneerRecipeStation::Fabricator;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recipe")
	TArray<FRecipeIngredient> Inputs;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recipe")
	TArray<FRecipeIngredient> Outputs;

	/** Seconds taken to complete one craft cycle (modified by station speed). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recipe")
	float ProcessTime = 2.f;

	/** Watts consumed during crafting. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recipe")
	float PowerCost = 100.f;

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId(TEXT("Recipe"), RecipeId);
	}
};
