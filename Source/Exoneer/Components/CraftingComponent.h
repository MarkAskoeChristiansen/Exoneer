// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Data/RecipeDefinitionDataAsset.h"
#include "CraftingComponent.generated.h"

class UInventoryComponent;
class UPowerComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCraftProgress, float, Progress);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnQueueChanged, int32, QueueSize);

/**
 * Generic crafting state machine attached to any production block
 * (refinery, fabricator, oxygen generator, food printer, ...).
 *
 * Maintains a queue of recipes; consumes inputs from a source inventory,
 * processes for ProcessTime (scaled by SpeedMultiplier and by the block's
 * power supply), and deposits outputs into a destination inventory.
 */
UCLASS(ClassGroup = (Exoneer), meta = (BlueprintSpawnableComponent))
class EXONEER_API UCraftingComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCraftingComponent();

	/** Only recipes that match this station type are accepted. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crafting") EExoneerRecipeStation StationType = EExoneerRecipeStation::Fabricator;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crafting") float SpeedMultiplier = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crafting") UInventoryComponent* InputInventory = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crafting") UInventoryComponent* OutputInventory = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crafting") UPowerComponent* PowerSource = nullptr;

	UPROPERTY(BlueprintAssignable) FOnCraftProgress OnCraftProgress;
	UPROPERTY(BlueprintAssignable) FOnQueueChanged OnQueueChanged;

	UFUNCTION(BlueprintCallable, Category = "Crafting") bool Enqueue(URecipeDefinitionDataAsset* Recipe);
	UFUNCTION(BlueprintCallable, Category = "Crafting") void ClearQueue();
	UFUNCTION(BlueprintPure, Category = "Crafting") int32 GetQueueSize() const { return Queue.Num(); }
	UFUNCTION(BlueprintPure, Category = "Crafting") float GetProgress() const { return CurrentProgress; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn) override;

protected:
	UPROPERTY() TArray<URecipeDefinitionDataAsset*> Queue;
	UPROPERTY() URecipeDefinitionDataAsset* Active = nullptr;
	float ActiveTime = 0.f;
	float CurrentProgress = 0.f;
	bool bConsumed = false;
};
