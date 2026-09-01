// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GameplayTagContainer.h"
#include "ExoneerBootstrapLibrary.generated.h"

/**
 * Editor/scripting helpers. Python cannot construct FGameplayTag directly
 * (TagName is read-only through the reflection path), so the content
 * bootstrap script builds tags and containers through these.
 */
UCLASS()
class EXONEER_API UExoneerBootstrapLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Resolve a registered tag by name ("Exoneer.Mount.Wall"). Invalid tag if unknown. */
	UFUNCTION(BlueprintPure, Category = "Exoneer|Bootstrap")
	static FGameplayTag MakeTag(FName TagName);

	UFUNCTION(BlueprintPure, Category = "Exoneer|Bootstrap")
	static FGameplayTagContainer MakeTagContainer(const TArray<FName>& TagNames);

	UFUNCTION(BlueprintPure, Category = "Exoneer|Bootstrap")
	static bool IsTagValid(FGameplayTag Tag) { return Tag.IsValid(); }
};
