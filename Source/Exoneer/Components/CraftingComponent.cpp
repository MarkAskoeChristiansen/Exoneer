// Copyright Exoneer contributors.
#include "Components/CraftingComponent.h"
#include "Components/InventoryComponent.h"
#include "Components/PowerComponent.h"
#include "Data/ItemDefinitionDataAsset.h"
#include "Interfaces/Constructible.h"
#include "GameFramework/Actor.h"
#include "Net/UnrealNetwork.h"

namespace
{
	/**
	 * Dry-run fit check: would ALL of the recipe's outputs fit into Out right
	 * now? Uses the inventory's own capacity model (volume or weight footprints
	 * against GetCurrentLoad); stack limits never cap totals, only stack splits.
	 */
	bool CanFitOutputs(const UInventoryComponent* Out, const URecipeDefinitionDataAsset* Recipe)
	{
		if (!Out || !Recipe)
		{
			return true;
		}
		if (Out->MaxCapacity <= 0.f)
		{
			return true;   // 0 = unlimited
		}

		float AddedFootprint = 0.f;
		for (const FRecipeIngredient& Output : Recipe->Outputs)
		{
			const UItemDefinitionDataAsset* Item = Output.Item.LoadSynchronous();
			if (!Item || Output.Count <= 0)
			{
				continue;
			}
			AddedFootprint += (Out->bUseWeight ? Item->Mass : Item->Volume) * Output.Count;
		}
		return Out->GetCurrentLoad() + AddedFootprint <= Out->MaxCapacity + KINDA_SMALL_NUMBER;
	}
}

UCraftingComponent::UCraftingComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.1f;
	SetIsReplicatedByDefault(true);
}

bool UCraftingComponent::Enqueue(URecipeDefinitionDataAsset* Recipe)
{
	const AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority())
	{
		UE_LOG(LogTemp, Warning, TEXT("UCraftingComponent::Enqueue called without authority; route through UInteractionComponent."));
		return false;
	}
	if (!Recipe || Recipe->Station != StationType)
	{
		return false;
	}
	if (Queue.Num() >= MaxQueueSize)
	{
		return false;
	}

	FCraftQueueEntry Entry;
	Entry.Recipe = Recipe;
	Entry.Progress01 = 0.f;
	Queue.Add(Entry);

	OnQueueChanged.Broadcast(Queue.Num());
	return true;
}

void UCraftingComponent::ClearQueue()
{
	const AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority())
	{
		UE_LOG(LogTemp, Warning, TEXT("UCraftingComponent::ClearQueue called without authority; route through UInteractionComponent."));
		return;
	}

	// Return the head craft's already-consumed inputs so items are not lost.
	if (bHeadConsumed && Queue.Num() > 0 && Queue[0].Recipe && InputInventory)
	{
		for (const FRecipeIngredient& Input : Queue[0].Recipe->Inputs)
		{
			if (UItemDefinitionDataAsset* Item = Input.Item.LoadSynchronous())
			{
				InputInventory->AddItem(Item, Input.Count);
			}
		}
	}

	Queue.Reset();
	bHeadConsumed = false;
	bOutputBlocked = false;
	OnQueueChanged.Broadcast(0);
	OnCraftProgress.Broadcast(0.f);
}

EMachineState UCraftingComponent::ComputeMachineState() const
{
	if (PowerSource && PowerSource->SupplyFraction < 0.5f)
	{
		return EMachineState::LowPower;
	}
	if (bOutputBlocked)
	{
		return EMachineState::OutputFull;
	}
	if (Queue.Num() > 0)
	{
		return EMachineState::Processing;
	}
	return EMachineState::Idle;
}

void UCraftingComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn)
{
	Super::TickComponent(DeltaTime, TickType, TickFn);

	AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority() || Queue.Num() == 0)
	{
		return;
	}

	// Machines are inert until their construction completes.
	if (Owner->Implements<UConstructible>() &&
		IConstructible::Execute_GetConstructionPhaseAt(Owner, Owner->GetActorLocation()) != EConstructionPhase::Complete)
	{
		return;
	}

	FCraftQueueEntry& Head = Queue[0];
	URecipeDefinitionDataAsset* Recipe = Head.Recipe;
	if (!Recipe)
	{
		Queue.RemoveAt(0);
		bHeadConsumed = false;
		bOutputBlocked = false;
		OnQueueChanged.Broadcast(Queue.Num());
		return;
	}

	const float PowerFrac = PowerSource ? FMath::Clamp(PowerSource->SupplyFraction, 0.f, 1.f) : 1.f;
	if (PowerFrac <= KINDA_SMALL_NUMBER && !bOutputBlocked)
	{
		return;   // no power: freeze progress, do not lock up materials
	}

	// Consume the head craft's inputs exactly once, when it starts.
	if (!bHeadConsumed)
	{
		if (!InputInventory)
		{
			return;
		}
		TArray<FInventoryEntry> Required;
		Required.Reserve(Recipe->Inputs.Num());
		for (const FRecipeIngredient& Input : Recipe->Inputs)
		{
			FInventoryEntry Entry;
			Entry.Item = Input.Item;
			Entry.Count = Input.Count;
			Required.Add(Entry);
		}
		if (!InputInventory->ConsumeItems(Required))
		{
			return;   // wait for materials (conveyors may still deliver them)
		}
		bHeadConsumed = true;
	}

	// Under-powered machines run slower; they do not fail.
	if (Head.Progress01 < 1.f)
	{
		const float ProcessTime = FMath::Max(Recipe->ProcessTime, KINDA_SMALL_NUMBER);
		Head.Progress01 = FMath::Min(1.f, Head.Progress01 + DeltaTime * SpeedMultiplier * PowerFrac / ProcessTime);
		OnCraftProgress.Broadcast(Head.Progress01);
	}

	if (Head.Progress01 < 1.f)
	{
		return;
	}

	// Finished: hold at 100% without depositing anything until ALL outputs fit.
	if (OutputInventory && !CanFitOutputs(OutputInventory, Recipe))
	{
		bOutputBlocked = true;
		return;
	}
	bOutputBlocked = false;

	if (OutputInventory)
	{
		for (const FRecipeIngredient& Output : Recipe->Outputs)
		{
			if (UItemDefinitionDataAsset* Item = Output.Item.LoadSynchronous())
			{
				const int32 Leftover = OutputInventory->AddItem(Item, Output.Count);
				if (Leftover > 0)
				{
					// The dry run said everything fits; log if reality disagrees.
					UE_LOG(LogTemp, Warning, TEXT("UCraftingComponent: %d x %s did not fit after fit check."), Leftover, *Item->GetName());
				}
			}
		}
	}

	Queue.RemoveAt(0);
	bHeadConsumed = false;
	OnQueueChanged.Broadcast(Queue.Num());
	OnCraftProgress.Broadcast(Queue.Num() > 0 ? Queue[0].Progress01 : 0.f);
}

void UCraftingComponent::OnRep_Queue()
{
	OnQueueChanged.Broadcast(Queue.Num());
	OnCraftProgress.Broadcast(Queue.Num() > 0 ? Queue[0].Progress01 : 0.f);
}

void UCraftingComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UCraftingComponent, Queue);
}
