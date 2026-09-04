// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ExoneerTypes.h"
#include "ProjectDefinitionDataAsset.generated.h"

/**
 * Optional project (Long Watch, Handshake, Road to Orbit). Never grants
 * tools. Author under /Game/Exoneer/Data/Projects/.
 */
UCLASS(BlueprintType)
class EXONEER_API UProjectDefinitionDataAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AssetRegistrySearchable, Category = "Project")
	FName ProjectId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Project")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Project", meta = (MultiLine = "true"))
	FText Brief;

	/** Empty = always offerable. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Project")
	TArray<FName> Prerequisites;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Project")
	TArray<FProjectCriterion> Criteria;

	/** How many sols the Active window must hold (Long Watch). 0 = instant check. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Project")
	int32 DurationSols = 0;

	/** Handshake writes orbital knowledge; others grant nothing. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Project")
	bool bGrantsOrbitalKnowledge = false;

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId(TEXT("Project"), ProjectId);
	}
};
