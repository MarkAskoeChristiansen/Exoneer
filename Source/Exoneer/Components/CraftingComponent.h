// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Data/RecipeDefinitionDataAsset.h"
#include "ExoneerTypes.h"
#include "CraftingComponent.generated.h"

class UInventoryComponent;
class UPowerComponent;

/** Compact replicated view of one queued craft, for machine UI. */
USTRUCT(BlueprintType)
struct FCraftQueueEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crafting")
	TObjectPtr<URecipeDefinitionDataAsset> Recipe = nullptr;

	/** Progress of THIS entry; only the head of the queue advances. */
	UPROPERTY(BlueprintReadOnly, Category = "Crafting")
	float Progress01 = 0.f;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCraftProgress, float, Progress);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnQueueChanged, int32, QueueSize);

/**
 * Generic crafting queue attached to production machines. SERVER simulates:
 * consumes inputs from InputInventory when a craft starts, advances by
 * DeltaSeconds * SpeedMultiplier * PowerSupplyFraction (under-powered machines
 * run slower, they do not fail), deposits outputs into OutputInventory.
 * If outputs do not fit, the craft holds at 100% until space frees up
 * (machine state: OutputFull).
 *
 * Client UI enqueues via UInteractionComponent::RequestEnqueueRecipe.
 * The queue replicates as FCraftQueueEntry summaries.
 */
UCLASS(ClassGroup = (Exoneer), meta = (BlueprintSpawnableComponent))
class EXONEER_API UCraftingComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCraftingComponent();

	/** Only recipes matching this station are accepted. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crafting") EExoneerRecipeStation StationType = EExoneerRecipeStation::Fabricator;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crafting") float SpeedMultiplier = 1.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crafting") int32 MaxQueueSize = 8;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crafting") TObjectPtr<UInventoryComponent> InputInventory;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crafting") TObjectPtr<UInventoryComponent> OutputInventory;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crafting") TObjectPtr<UPowerComponent> PowerSource;

	UPROPERTY(BlueprintAssignable) FOnCraftProgress OnCraftProgress;
	UPROPERTY(BlueprintAssignable) FOnQueueChanged OnQueueChanged;

	/** SERVER-ONLY (UI routes through UInteractionComponent). */
	UFUNCTION(BlueprintCallable, Category = "Crafting") bool Enqueue(URecipeDefinitionDataAsset* Recipe);
	UFUNCTION(BlueprintCallable, Category = "Crafting") void ClearQueue();

	UFUNCTION(BlueprintPure, Category = "Crafting") int32 GetQueueSize() const { return Queue.Num(); }
	UFUNCTION(BlueprintPure, Category = "Crafting") float GetProgress() const { return Queue.Num() > 0 ? Queue[0].Progress01 : 0.f; }
	UFUNCTION(BlueprintPure, Category = "Crafting") const TArray<FCraftQueueEntry>& GetQueue() const { return Queue; }

	/** Derived machine state for AMachinePiece (server computes each sim tick). */
	UFUNCTION(BlueprintPure, Category = "Crafting") EMachineState ComputeMachineState() const;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	UPROPERTY(ReplicatedUsing = OnRep_Queue)
	TArray<FCraftQueueEntry> Queue;

	/** True once the head craft's inputs were consumed. */
	bool bHeadConsumed = false;

	/** True while the head craft is finished but its outputs do not fit. */
	bool bOutputBlocked = false;

	UFUNCTION() void OnRep_Queue();
};
