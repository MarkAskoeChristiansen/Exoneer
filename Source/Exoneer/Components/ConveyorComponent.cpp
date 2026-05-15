// Copyright Exoneer contributors.
#include "Components/ConveyorComponent.h"
#include "Components/InventoryComponent.h"
#include "Interfaces/InventoryOwner.h"
#include "EngineUtils.h"

UConveyorComponent::UConveyorComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UConveyorComponent::GatherNearbyInventories(TArray<UInventoryComponent*>& OutInventories) const
{
	const AActor* Self = GetOwner();
	if (!Self || !GetWorld()) return;
	const FVector Origin = Self->GetActorLocation();
	const float R2 = ConnectionRange * ConnectionRange;

	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		AActor* A = *It;
		if (!A || A == Self) continue;
		if (FVector::DistSquared(A->GetActorLocation(), Origin) > R2) continue;

		UInventoryComponent* Inv = nullptr;
		if (A->Implements<UInventoryOwner>())
		{
			Inv = IInventoryOwner::Execute_GetInventory(A);
		}
		if (!Inv)
		{
			Inv = A->FindComponentByClass<UInventoryComponent>();
		}
		if (Inv) OutInventories.AddUnique(Inv);
	}
}

int32 UConveyorComponent::PullInto(UInventoryComponent* Destination, UItemDefinitionDataAsset* Item, int32 Count)
{
	if (!Destination || !Item || Count <= 0) return 0;
	TArray<UInventoryComponent*> Sources;
	GatherNearbyInventories(Sources);

	int32 Pulled = 0;
	for (UInventoryComponent* Src : Sources)
	{
		if (Src == Destination) continue;
		const int32 WantLeft = Count - Pulled;
		if (WantLeft <= 0) break;
		const int32 Taken = Src->RemoveItem(Item, FMath::Min(Src->GetItemCount(Item), WantLeft));
		const int32 Leftover = Destination->AddItem(Item, Taken);
		// Return un-acceptable leftover to source.
		if (Leftover > 0) Src->AddItem(Item, Leftover);
		Pulled += (Taken - Leftover);
	}
	return Pulled;
}

int32 UConveyorComponent::PushFrom(UInventoryComponent* Source, UItemDefinitionDataAsset* Item, int32 Count)
{
	if (!Source || !Item || Count <= 0) return 0;
	TArray<UInventoryComponent*> Dests;
	GatherNearbyInventories(Dests);

	int32 Pushed = 0;
	for (UInventoryComponent* Dst : Dests)
	{
		if (Dst == Source) continue;
		const int32 WantLeft = Count - Pushed;
		if (WantLeft <= 0) break;
		const int32 ToMove = FMath::Min(Source->GetItemCount(Item), WantLeft);
		if (ToMove <= 0) continue;
		Source->RemoveItem(Item, ToMove);
		const int32 Leftover = Dst->AddItem(Item, ToMove);
		if (Leftover > 0) Source->AddItem(Item, Leftover);
		Pushed += (ToMove - Leftover);
	}
	return Pushed;
}
