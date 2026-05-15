// Copyright Exoneer contributors.
#include "Components/CraftingComponent.h"
#include "Components/InventoryComponent.h"
#include "Components/PowerComponent.h"
#include "Data/ItemDefinitionDataAsset.h"

UCraftingComponent::UCraftingComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.1f;
}

bool UCraftingComponent::Enqueue(URecipeDefinitionDataAsset* Recipe)
{
	if (!Recipe || Recipe->Station != StationType) return false;
	Queue.Add(Recipe);
	OnQueueChanged.Broadcast(Queue.Num());
	return true;
}

void UCraftingComponent::ClearQueue()
{
	Queue.Reset();
	Active = nullptr;
	ActiveTime = 0.f;
	CurrentProgress = 0.f;
	bConsumed = false;
	OnQueueChanged.Broadcast(0);
	OnCraftProgress.Broadcast(0.f);
}

void UCraftingComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn)
{
	Super::TickComponent(DeltaTime, TickType, TickFn);

	if (!Active && Queue.Num() > 0)
	{
		Active = Queue[0];
		Queue.RemoveAt(0);
		ActiveTime = 0.f;
		CurrentProgress = 0.f;
		bConsumed = false;
		OnQueueChanged.Broadcast(Queue.Num());
	}
	if (!Active) return;

	const float PowerFrac = PowerSource ? FMath::Clamp(PowerSource->SupplyFraction, 0.f, 1.f) : 1.f;
	if (PowerFrac <= KINDA_SMALL_NUMBER) return; // No power → freeze progress

	// Consume inputs at start.
	if (!bConsumed)
	{
		if (!InputInventory) return;
		TArray<FInventoryEntry> Req;
		Req.Reserve(Active->Inputs.Num());
		for (const FRecipeIngredient& In : Active->Inputs)
		{
			Req.Add({ In.Item, In.Count });
		}
		if (!InputInventory->ConsumeItems(Req))
		{
			// Skip this recipe; can't satisfy inputs.
			Active = nullptr;
			return;
		}
		bConsumed = true;
	}

	const float EffectiveDT = DeltaTime * SpeedMultiplier * PowerFrac;
	ActiveTime += EffectiveDT;
	CurrentProgress = (Active->ProcessTime > 0.f) ? FMath::Clamp(ActiveTime / Active->ProcessTime, 0.f, 1.f) : 1.f;
	OnCraftProgress.Broadcast(CurrentProgress);

	if (ActiveTime >= Active->ProcessTime)
	{
		// Deposit outputs.
		if (OutputInventory)
		{
			for (const FRecipeIngredient& Out : Active->Outputs)
			{
				if (UItemDefinitionDataAsset* I = Out.Item.LoadSynchronous())
				{
					OutputInventory->AddItem(I, Out.Count);
				}
			}
		}
		Active = nullptr;
		ActiveTime = 0.f;
		CurrentProgress = 0.f;
		bConsumed = false;
		OnCraftProgress.Broadcast(0.f);
	}
}
