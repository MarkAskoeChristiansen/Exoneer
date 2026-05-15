// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "Buildable.generated.h"

UINTERFACE(BlueprintType, MinimalAPI)
class UBuildable : public UInterface { GENERATED_BODY() };

class UBlockDefinitionDataAsset;

/**
 * Actors that participate in the grid-building system.
 */
class EXONEER_API IBuildable
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Build")
	void OnPlaced(const FIntVector& GridCoord);
	virtual void OnPlaced_Implementation(const FIntVector& GridCoord) {}

	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Build")
	void OnRemoved();
	virtual void OnRemoved_Implementation() {}

	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Build")
	UBlockDefinitionDataAsset* GetBlockDefinition() const;
	virtual UBlockDefinitionDataAsset* GetBlockDefinition_Implementation() const { return nullptr; }
};
