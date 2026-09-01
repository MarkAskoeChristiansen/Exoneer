// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Net/Serialization/FastArraySerializer.h"
#include "ExoneerTypes.h"
#include "InventoryComponent.generated.h"

class UItemDefinitionDataAsset;
class UInventoryComponent;

/**
 * One replicated stack of a single item type. Runtime counterpart of the
 * authored/save-side FInventoryEntry.
 */
USTRUCT(BlueprintType)
struct FInventoryStack : public FFastArraySerializerItem
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Inventory")
	TObjectPtr<UItemDefinitionDataAsset> Item = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "Inventory")
	int32 Count = 0;

	// Fast array client callbacks; forward change notifications to the owner.
	void PreReplicatedRemove(const struct FInventoryList& InArray);
	void PostReplicatedAdd(const struct FInventoryList& InArray);
	void PostReplicatedChange(const struct FInventoryList& InArray);
};

/** Delta-serialized stack container. */
USTRUCT()
struct FInventoryList : public FFastArraySerializer
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FInventoryStack> Stacks;

	/** Owning component, for client-side change broadcasts. Not replicated. */
	UPROPERTY(NotReplicated)
	TObjectPtr<UInventoryComponent> OwnerComponent = nullptr;

	bool NetDeltaSerialize(FNetDeltaSerializeInfo& DeltaParms)
	{
		return FFastArraySerializer::FastArrayDeltaSerialize<FInventoryStack, FInventoryList>(Stacks, DeltaParms, *this);
	}

	/**
	 * Fires once per received delta batch, AFTER removals/adds/changes are
	 * applied. The per-item callbacks must not broadcast: PreReplicatedRemove
	 * runs before the stack is gone, so UI reading the array there would still
	 * see deleted stacks.
	 */
	void PostReplicatedReceive(const FFastArraySerializer::FPostReplicatedReceiveParameters& Parameters);

	void BroadcastChanged() const;
};

template<>
struct TStructOpsTypeTraits<FInventoryList> : public TStructOpsTypeTraitsBase2<FInventoryList>
{
	enum { WithNetDeltaSerializer = true };
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnInventoryChanged);

/**
 * Generic replicated inventory used by the player, machines, and cargo blocks.
 *
 * Capacity model:
 *  - bUseWeight = true  => capacity in kg (player suit)
 *  - bUseWeight = false => capacity in volume units (cargo/machine buffers)
 *  - MaxCapacity == 0   => unlimited (debug)
 *
 * Authority model: AddItem/RemoveItem/ConsumeItems mutate ONLY on the server
 * (they fail with a log on clients). Client UI moves items between containers
 * with RequestTransfer, which routes through a Server RPC on the local
 * player's own inventory component and is range-validated server-side.
 */
UCLASS(ClassGroup = (Exoneer), meta = (BlueprintSpawnableComponent))
class EXONEER_API UInventoryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UInventoryComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Replicated, Category = "Inventory")
	float MaxCapacity = 100.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Replicated, Category = "Inventory")
	bool bUseWeight = true;

	/** Max distance between the requesting pawn and both containers for transfers. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Inventory")
	float TransferReach = 600.f;

	/** Fires on the server after any mutation, and on clients after replication. */
	UPROPERTY(BlueprintAssignable, Category = "Inventory")
	FOnInventoryChanged OnInventoryChanged;

	// --- Mutations (SERVER-ONLY; fail with a warning on clients) ---

	/** Try to add Count of Item. Returns the amount that did NOT fit. */
	UFUNCTION(BlueprintCallable, Category = "Inventory")
	int32 AddItem(UItemDefinitionDataAsset* Item, int32 Count);

	/** Remove up to Count of Item. Returns amount actually removed. */
	UFUNCTION(BlueprintCallable, Category = "Inventory")
	int32 RemoveItem(UItemDefinitionDataAsset* Item, int32 Count);

	/** Atomically consume the requested items if all are present. */
	UFUNCTION(BlueprintCallable, Category = "Inventory")
	bool ConsumeItems(const TArray<FInventoryEntry>& Required);

	// --- Queries (safe everywhere; clients see replicated state) ---

	UFUNCTION(BlueprintCallable, Category = "Inventory")
	int32 GetItemCount(UItemDefinitionDataAsset* Item) const;

	UFUNCTION(BlueprintCallable, Category = "Inventory")
	bool HasItems(const TArray<FInventoryEntry>& Required) const;

	UFUNCTION(BlueprintPure, Category = "Inventory")
	float GetCurrentLoad() const;

	UFUNCTION(BlueprintPure, Category = "Inventory")
	float GetLoadFraction() const;

	/** Soft-pointer snapshot for save games and legacy callers. Built per call. */
	UFUNCTION(BlueprintPure, Category = "Inventory")
	TArray<FInventoryEntry> GetEntries() const;

	/** Direct runtime stacks for UI list views. */
	UFUNCTION(BlueprintPure, Category = "Inventory")
	const TArray<FInventoryStack>& GetStacks() const { return List.Stacks; }

	// --- Container transfer ---

	/**
	 * Move Count of Item from Source to Target. Callable from client UI, but
	 * ONLY on the local player's own inventory component (its owner carries
	 * the network connection); Source/Target may be any two inventories in
	 * reach, this component included. Executes directly on authority.
	 */
	UFUNCTION(BlueprintCallable, Category = "Inventory")
	void RequestTransfer(UInventoryComponent* Source, UInventoryComponent* Target, UItemDefinitionDataAsset* Item, int32 Count);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	UPROPERTY(Replicated)
	FInventoryList List;

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_RequestTransfer(UInventoryComponent* Source, UInventoryComponent* Target, UItemDefinitionDataAsset* Item, int32 Count);

	/** SERVER. The validated transfer: leftover that does not fit returns to Source. */
	static void ExecuteTransfer(UInventoryComponent* Source, UInventoryComponent* Target, UItemDefinitionDataAsset* Item, int32 Count);

	/** Per-unit cost (kg or volume) for the given item under this capacity model. */
	float GetUnitFootprint(const UItemDefinitionDataAsset* Item) const;

	bool HasAuthority() const;

	friend struct FInventoryStack;
	friend struct FInventoryList;
};
