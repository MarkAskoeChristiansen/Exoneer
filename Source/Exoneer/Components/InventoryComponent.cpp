// Copyright Exoneer contributors.
#include "Components/InventoryComponent.h"
#include "Data/ItemDefinitionDataAsset.h"

UInventoryComponent::UInventoryComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

float UInventoryComponent::GetUnitFootprint(UItemDefinitionDataAsset* Item) const
{
	if (!Item) return 0.f;
	return bUseWeight ? Item->Mass : Item->Volume;
}

float UInventoryComponent::GetCurrentLoad() const
{
	float Total = 0.f;
	for (const FInventoryEntry& E : Entries)
	{
		UItemDefinitionDataAsset* I = E.Item.LoadSynchronous();
		Total += GetUnitFootprint(I) * E.Count;
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

	const float UnitFootprint = GetUnitFootprint(Item);
	int32 Remaining = Count;

	// Capacity check.
	if (MaxCapacity > 0.f && UnitFootprint > 0.f)
	{
		const float Free = FMath::Max(0.f, MaxCapacity - GetCurrentLoad());
		const int32 MaxByCapacity = FMath::FloorToInt(Free / UnitFootprint);
		Remaining = FMath::Min(Remaining, MaxByCapacity);
		if (Remaining <= 0) return Count;
	}

	const int32 Granted = Remaining;

	// Fill existing stacks first.
	for (FInventoryEntry& E : Entries)
	{
		if (Remaining <= 0) break;
		if (E.Item == Item)
		{
			const int32 SpaceInStack = FMath::Max(0, Item->MaxStack - E.Count);
			const int32 ToAdd = FMath::Min(SpaceInStack, Remaining);
			E.Count += ToAdd;
			Remaining -= ToAdd;
		}
	}

	// Create new stacks.
	while (Remaining > 0)
	{
		const int32 ToAdd = FMath::Min(Item->MaxStack, Remaining);
		Entries.Add({ Item, ToAdd });
		Remaining -= ToAdd;
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
	int32 Remaining = Count;

	for (int32 i = Entries.Num() - 1; i >= 0 && Remaining > 0; --i)
	{
		FInventoryEntry& E = Entries[i];
		if (E.Item == Item)
		{
			const int32 Taken = FMath::Min(E.Count, Remaining);
			E.Count -= Taken;
			Remaining -= Taken;
			if (E.Count <= 0)
			{
				Entries.RemoveAt(i);
			}
		}
	}

	const int32 Removed = Count - Remaining;
	if (Removed > 0)
	{
		OnInventoryChanged.Broadcast();
	}
	return Removed;
}

int32 UInventoryComponent::GetItemCount(UItemDefinitionDataAsset* Item) const
{
	if (!Item) return 0;
	int32 Total = 0;
	for (const FInventoryEntry& E : Entries)
	{
		if (E.Item == Item) Total += E.Count;
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

bool UInventoryComponent::ConsumeItems(const TArray<FInventoryEntry>& Required)
{
	if (!HasItems(Required)) return false;
	for (const FInventoryEntry& Req : Required)
	{
		RemoveItem(Req.Item.LoadSynchronous(), Req.Count);
	}
	return true;
}
