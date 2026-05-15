// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "InventoryComponent.generated.h"

class UItemDefinitionDataAsset;

USTRUCT(BlueprintType)
struct FInventoryEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Inventory")
	TSoftObjectPtr<UItemDefinitionDataAsset> Item;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Inventory")
	int32 Count = 0;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnInventoryChanged);

/**
 * Generic inventory used by the player, machines, and cargo containers.
 *
 * Capacity model:
 *  - bUseWeight = true ⇒ capacity in kg (player suit, vehicles)
 *  - bUseWeight = false ⇒ capacity in volume units (cargo blocks)
 *  - MaxCapacity == 0 ⇒ unlimited (debug)
 */
UCLASS(ClassGroup = (Exoneer), meta = (BlueprintSpawnableComponent))
class EXONEER_API UInventoryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UInventoryComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Inventory")
	float MaxCapacity = 100.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Inventory")
	bool bUseWeight = true;

	UPROPERTY(BlueprintAssignable, Category = "Inventory")
	FOnInventoryChanged OnInventoryChanged;

	/** Try to add Count of Item to the inventory. Returns the amount that did NOT fit. */
	UFUNCTION(BlueprintCallable, Category = "Inventory")
	int32 AddItem(UItemDefinitionDataAsset* Item, int32 Count);

	/** Remove up to Count of Item. Returns amount actually removed. */
	UFUNCTION(BlueprintCallable, Category = "Inventory")
	int32 RemoveItem(UItemDefinitionDataAsset* Item, int32 Count);

	UFUNCTION(BlueprintCallable, Category = "Inventory")
	int32 GetItemCount(UItemDefinitionDataAsset* Item) const;

	UFUNCTION(BlueprintCallable, Category = "Inventory")
	bool HasItems(const TArray<FInventoryEntry>& Required) const;

	/** Atomically consume the requested items if all are present. */
	UFUNCTION(BlueprintCallable, Category = "Inventory")
	bool ConsumeItems(const TArray<FInventoryEntry>& Required);

	UFUNCTION(BlueprintPure, Category = "Inventory")
	float GetCurrentLoad() const;

	UFUNCTION(BlueprintPure, Category = "Inventory")
	float GetLoadFraction() const;

	UFUNCTION(BlueprintPure, Category = "Inventory")
	const TArray<FInventoryEntry>& GetEntries() const { return Entries; }

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Inventory")
	TArray<FInventoryEntry> Entries;

	/** Returns the per-unit cost (kg or volume) for the given item. */
	float GetUnitFootprint(UItemDefinitionDataAsset* Item) const;
};
