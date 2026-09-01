// Copyright Exoneer contributors.
#include "Components/InventoryComponent.h"
#include "Exoneer.h"
#include "Data/ItemDefinitionDataAsset.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"

// --- Fast array client callbacks -------------------------------------------
// The per-item callbacks stay silent: PreReplicatedRemove runs BEFORE the
// stack leaves the array, so broadcasting there hands the UI deleted stacks.
// PostReplicatedReceive fires once per delta batch after everything applied.

void FInventoryStack::PreReplicatedRemove(const FInventoryList& InArray)
{
}

void FInventoryStack::PostReplicatedAdd(const FInventoryList& InArray)
{
}

void FInventoryStack::PostReplicatedChange(const FInventoryList& InArray)
{
}

void FInventoryList::PostReplicatedReceive(const FFastArraySerializer::FPostReplicatedReceiveParameters& Parameters)
{
	BroadcastChanged();
}

void FInventoryList::BroadcastChanged() const
{
	if (OwnerComponent)
	{
		OwnerComponent->OnInventoryChanged.Broadcast();
	}
}

// --- Component --------------------------------------------------------------

UInventoryComponent::UInventoryComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
	List.OwnerComponent = this;
}

void UInventoryComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UInventoryComponent, List);
	DOREPLIFETIME(UInventoryComponent, MaxCapacity);
	DOREPLIFETIME(UInventoryComponent, bUseWeight);
}

bool UInventoryComponent::HasAuthority() const
{
	const AActor* Owner = GetOwner();
	return Owner && Owner->HasAuthority();
}

float UInventoryComponent::GetUnitFootprint(const UItemDefinitionDataAsset* Item) const
{
	if (!Item) return 0.f;
	return bUseWeight ? Item->Mass : Item->Volume;
}

float UInventoryComponent::GetCurrentLoad() const
{
	float Total = 0.f;
	for (const FInventoryStack& Stack : List.Stacks)
	{
		Total += GetUnitFootprint(Stack.Item) * Stack.Count;
	}
	return Total;
}

float UInventoryComponent::GetLoadFraction() const
{
	return (MaxCapacity > 0.f) ? FMath::Clamp(GetCurrentLoad() / MaxCapacity, 0.f, 1.f) : 0.f;
}

int32 UInventoryComponent::AddItem(UItemDefinitionDataAsset* Item, int32 Count)
{
	if (!Item || Count <= 0) return Count;

	if (!HasAuthority())
	{
		UE_LOG(LogExoneer, Warning, TEXT("AddItem(%s x%d) called without authority on %s; ignored."),
			*Item->ItemId.ToString(), Count, *GetNameSafe(GetOwner()));
		return Count;
	}

	int32 Remaining = Count;

	// Capacity check: only whole units fit under the weight/volume budget.
	const float UnitFootprint = GetUnitFootprint(Item);
	if (MaxCapacity > 0.f && UnitFootprint > 0.f)
	{
		const float Free = FMath::Max(0.f, MaxCapacity - GetCurrentLoad());
		const int32 MaxByCapacity = FMath::FloorToInt(Free / UnitFootprint);
		Remaining = FMath::Min(Remaining, MaxByCapacity);
		if (Remaining <= 0) return Count;
	}

	const int32 Granted = Remaining;
	const int32 MaxStack = FMath::Max(1, Item->MaxStack);

	// Fill existing stacks first.
	for (FInventoryStack& Stack : List.Stacks)
	{
		if (Remaining <= 0) break;
		if (Stack.Item != Item) continue;

		const int32 SpaceInStack = FMath::Max(0, MaxStack - Stack.Count);
		const int32 ToAdd = FMath::Min(SpaceInStack, Remaining);
		if (ToAdd > 0)
		{
			Stack.Count += ToAdd;
			Remaining -= ToAdd;
			List.MarkItemDirty(Stack);
		}
	}

	// Open new stacks for the rest.
	while (Remaining > 0)
	{
		FInventoryStack& NewStack = List.Stacks.AddDefaulted_GetRef();
		NewStack.Item = Item;
		NewStack.Count = FMath::Min(MaxStack, Remaining);
		Remaining -= NewStack.Count;
		List.MarkItemDirty(NewStack);
	}

	if (Granted > 0)
	{
		OnInventoryChanged.Broadcast();
	}
	return Count - Granted;
}

int32 UInventoryComponent::RemoveItem(UItemDefinitionDataAsset* Item, int32 Count)
{
	if (!Item || Count <= 0) return 0;

	if (!HasAuthority())
	{
		UE_LOG(LogExoneer, Warning, TEXT("RemoveItem(%s x%d) called without authority on %s; ignored."),
			*Item->ItemId.ToString(), Count, *GetNameSafe(GetOwner()));
		return 0;
	}

	int32 Remaining = Count;
	bool bRemovedStacks = false;

	for (int32 i = List.Stacks.Num() - 1; i >= 0 && Remaining > 0; --i)
	{
		FInventoryStack& Stack = List.Stacks[i];
		if (Stack.Item != Item) continue;

		const int32 Taken = FMath::Min(Stack.Count, Remaining);
		Stack.Count -= Taken;
		Remaining -= Taken;
		if (Stack.Count <= 0)
		{
			List.Stacks.RemoveAt(i);
			bRemovedStacks = true;
		}
		else
		{
			List.MarkItemDirty(Stack);
		}
	}

	if (bRemovedStacks)
	{
		List.MarkArrayDirty();
	}

	const int32 Removed = Count - Remaining;
	if (Removed > 0)
	{
		OnInventoryChanged.Broadcast();
	}
	return Removed;
}

bool UInventoryComponent::ConsumeItems(const TArray<FInventoryEntry>& Required)
{
	if (!HasAuthority())
	{
		UE_LOG(LogExoneer, Warning, TEXT("ConsumeItems called without authority on %s; ignored."),
			*GetNameSafe(GetOwner()));
		return false;
	}

	if (!HasItems(Required)) return false;

	for (const FInventoryEntry& Req : Required)
	{
		RemoveItem(Req.Item.LoadSynchronous(), Req.Count);
	}
	return true;
}

int32 UInventoryComponent::GetItemCount(UItemDefinitionDataAsset* Item) const
{
	if (!Item) return 0;
	int32 Total = 0;
	for (const FInventoryStack& Stack : List.Stacks)
	{
		if (Stack.Item == Item) Total += Stack.Count;
	}
	return Total;
}

bool UInventoryComponent::HasItems(const TArray<FInventoryEntry>& Required) const
{
	for (const FInventoryEntry& Req : Required)
	{
		if (GetItemCount(Req.Item.LoadSynchronous()) < Req.Count) return false;
	}
	return true;
}

TArray<FInventoryEntry> UInventoryComponent::GetEntries() const
{
	TArray<FInventoryEntry> Entries;
	Entries.Reserve(List.Stacks.Num());
	for (const FInventoryStack& Stack : List.Stacks)
	{
		if (!Stack.Item || Stack.Count <= 0) continue;

		FInventoryEntry& Entry = Entries.AddDefaulted_GetRef();
		Entry.Item = Stack.Item.Get();
		Entry.Count = Stack.Count;
	}
	return Entries;
}

// --- Container transfer ------------------------------------------------------

void UInventoryComponent::RequestTransfer(UInventoryComponent* Source, UInventoryComponent* Target, UItemDefinitionDataAsset* Item, int32 Count)
{
	if (!Source || !Target || !Item || Count <= 0 || Source == Target) return;

	if (HasAuthority())
	{
		ExecuteTransfer(Source, Target, Item, Count);
		return;
	}

	// Client intent must ride this connection's own pawn (spec section 3).
	const APawn* Pawn = Cast<APawn>(GetOwner());
	if (!Pawn || !Pawn->IsLocallyControlled())
	{
		UE_LOG(LogExoneer, Warning, TEXT("RequestTransfer must be called on the local player's own InventoryComponent (owner: %s)."),
			*GetNameSafe(GetOwner()));
		return;
	}

	Server_RequestTransfer(Source, Target, Item, Count);
}

bool UInventoryComponent::Server_RequestTransfer_Validate(UInventoryComponent* Source, UInventoryComponent* Target, UItemDefinitionDataAsset* Item, int32 Count)
{
	// An honest client never sends a non-positive count. Object references can
	// legitimately fail to resolve under relevancy, so nulls soft-fail in the
	// implementation instead of disconnecting the sender.
	return Count > 0;
}

void UInventoryComponent::Server_RequestTransfer_Implementation(UInventoryComponent* Source, UInventoryComponent* Target, UItemDefinitionDataAsset* Item, int32 Count)
{
	if (!Source || !Target || !Item || Count <= 0) return;

	const APawn* Pawn = Cast<APawn>(GetOwner());
	const AActor* SourceOwner = Source->GetOwner();
	const AActor* TargetOwner = Target->GetOwner();
	if (!Pawn || !SourceOwner || !TargetOwner) return;

	// The requesting pawn must be within reach of BOTH containers.
	const FVector PawnLocation = Pawn->GetActorLocation();
	if (FVector::Dist(PawnLocation, SourceOwner->GetActorLocation()) > TransferReach ||
		FVector::Dist(PawnLocation, TargetOwner->GetActorLocation()) > TransferReach)
	{
		UE_LOG(LogExoneer, Verbose, TEXT("Transfer rejected: %s is out of reach of a container."), *GetNameSafe(Pawn));
		return;
	}

	ExecuteTransfer(Source, Target, Item, Count);
}

void UInventoryComponent::ExecuteTransfer(UInventoryComponent* Source, UInventoryComponent* Target, UItemDefinitionDataAsset* Item, int32 Count)
{
	const int32 Removed = Source->RemoveItem(Item, Count);
	if (Removed <= 0) return;

	// Whatever does not fit in the target goes straight back where it came from.
	const int32 Leftover = Target->AddItem(Item, Removed);
	if (Leftover > 0)
	{
		Source->AddItem(Item, Leftover);
	}
}
