// Copyright Exoneer contributors.
#include "Machines/MachinePiece.h"
#include "Components/PowerComponent.h"
#include "Components/InventoryComponent.h"
#include "Components/ConveyorComponent.h"
#include "Components/ConstructionComponent.h"
#include "Components/CraftingComponent.h"
#include "Data/PieceDefinitionDataAsset.h"
#include "Data/ItemDefinitionDataAsset.h"
#include "ExoneerGameplayTags.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

namespace
{
	/** All authored recipes for one station type, loaded via the AssetManager. */
	void GetStationRecipes(EExoneerRecipeStation Station, TArray<URecipeDefinitionDataAsset*>& Out)
	{
		UAssetManager& Manager = UAssetManager::Get();
		TArray<FPrimaryAssetId> Ids;
		Manager.GetPrimaryAssetIdList(FPrimaryAssetType(TEXT("Recipe")), Ids);
		for (const FPrimaryAssetId& Id : Ids)
		{
			const FSoftObjectPath Path = Manager.GetPrimaryAssetPath(Id);
			if (URecipeDefinitionDataAsset* Recipe = Cast<URecipeDefinitionDataAsset>(Path.TryLoad()))
			{
				if (Recipe->Station == Station)
				{
					Out.Add(Recipe);
				}
			}
		}
	}
}

AMachinePiece::AMachinePiece()
{
	Power = CreateDefaultSubobject<UPowerComponent>(TEXT("Power"));
	Inventory = CreateDefaultSubobject<UInventoryComponent>(TEXT("Inventory"));
	Inventory->bUseWeight = false;   // machine buffers are volume-based
	Conveyor = CreateDefaultSubobject<UConveyorComponent>(TEXT("Conveyor"));
}

void AMachinePiece::BeginPlay()
{
	Super::BeginPlay();

	if (!HasAuthority())
	{
		return;
	}

	ApplyDefinitionStats();

	// Machines are inert until construction completes. The power network only
	// registers COMPLETE pieces, but bEnabled gates the component regardless.
	const bool bComplete = Construction && Construction->IsComplete();
	if (Power)
	{
		Power->bEnabled = bComplete;
	}

	if (Construction && !bComplete)
	{
		// Construction->OnPhaseChanged is a dynamic delegate and this class
		// declares no UFUNCTION handler for it, so completion is polled on a
		// light server timer instead (cleared as soon as the piece completes).
		TSharedRef<FTimerHandle> Handle = MakeShared<FTimerHandle>();
		GetWorldTimerManager().SetTimer(*Handle, FTimerDelegate::CreateWeakLambda(this, [this, Handle]()
		{
			if (Construction && !Construction->IsComplete())
			{
				return;
			}
			// Re-apply stats: Def may have arrived after BeginPlay, and the
			// machine only starts drawing/producing power once complete.
			ApplyDefinitionStats();
			if (Power)
			{
				Power->bEnabled = true;
			}
			GetWorldTimerManager().ClearTimer(*Handle);
		}), 0.25f, true);
	}
}

void AMachinePiece::ApplyDefinitionStats()
{
	if (!Def)
	{
		return;
	}

	if (Power)
	{
		// PowerDelta: positive produces, negative consumes.
		Power->NominalDraw = Def->PowerDelta < 0.f ? -Def->PowerDelta : 0.f;
		Power->NominalOutput = Def->PowerDelta > 0.f ? Def->PowerDelta : 0.f;
		Power->StorageCapacity = Def->EnergyStorage;
	}
	if (Inventory)
	{
		Inventory->MaxCapacity = Def->InventoryCapacity;
	}
}

void AMachinePiece::UpdateMachineState(EMachineState NewState)
{
	if (!HasAuthority() || MachineState == NewState)
	{
		return;
	}
	MachineState = NewState;
	// RepNotify only fires on clients; mirror the broadcast on the server.
	OnRep_MachineState();
}

void AMachinePiece::OnRep_MachineState()
{
	OnMachineStateChanged.Broadcast(MachineState);
}

bool AMachinePiece::OnInteract_Implementation(AActor* Interactor)
{
	if (!Construction || !Construction->IsComplete())
	{
		return false;
	}

	// PROTOTYPE AFFORDANCE until the wrist computer UI exists (GAME-SCOPE.md
	// module 8): one interaction runs the whole exchange with a production
	// machine - withdraw finished goods, deposit matching raw inputs from the
	// player, and queue as many crafts as the buffer affords.
	UCraftingComponent* Crafting = FindComponentByClass<UCraftingComponent>();
	APawn* Pawn = Cast<APawn>(Interactor);
	UInventoryComponent* PlayerInv = Pawn ? Pawn->FindComponentByClass<UInventoryComponent>() : nullptr;
	if (!Crafting || !Inventory || !PlayerInv || !HasAuthority())
	{
		return true;   // Plain machines (battery, solar): interact just opens UI.
	}

	TArray<URecipeDefinitionDataAsset*> Recipes;
	GetStationRecipes(Crafting->StationType, Recipes);
	if (Recipes.Num() == 0)
	{
		return true;
	}

	TSet<UItemDefinitionDataAsset*> InputItems;
	TSet<UItemDefinitionDataAsset*> OutputItems;
	for (const URecipeDefinitionDataAsset* Recipe : Recipes)
	{
		for (const FRecipeIngredient& In : Recipe->Inputs)
		{
			if (UItemDefinitionDataAsset* Item = In.Item.LoadSynchronous()) InputItems.Add(Item);
		}
		for (const FRecipeIngredient& Out : Recipe->Outputs)
		{
			if (UItemDefinitionDataAsset* Item = Out.Item.LoadSynchronous()) OutputItems.Add(Item);
		}
	}

	// 1. Withdraw: finished goods (outputs that are not also inputs here).
	int32 Withdrawn = 0;
	for (UItemDefinitionDataAsset* Item : OutputItems)
	{
		if (InputItems.Contains(Item))
		{
			continue;
		}
		const int32 Count = Inventory->GetItemCount(Item);
		if (Count > 0)
		{
			const int32 Moved = Inventory->RemoveItem(Item, Count);
			const int32 Leftover = PlayerInv->AddItem(Item, Moved);
			if (Leftover > 0)
			{
				Inventory->AddItem(Item, Leftover);   // Player full: put it back.
			}
			Withdrawn += Moved - Leftover;
		}
	}

	// 2. Deposit: everything the player carries that this station consumes.
	int32 Deposited = 0;
	for (UItemDefinitionDataAsset* Item : InputItems)
	{
		const int32 Count = PlayerInv->GetItemCount(Item);
		if (Count > 0)
		{
			const int32 Moved = PlayerInv->RemoveItem(Item, Count);
			const int32 Leftover = Inventory->AddItem(Item, Moved);
			if (Leftover > 0)
			{
				PlayerInv->AddItem(Item, Leftover);   // Machine buffer full.
			}
			Deposited += Moved - Leftover;
		}
	}

	// 3. Queue: fill remaining queue slots with whatever is affordable now.
	int32 Queued = 0;
	while (Crafting->GetQueueSize() < Crafting->MaxQueueSize)
	{
		bool bQueuedAny = false;
		for (URecipeDefinitionDataAsset* Recipe : Recipes)
		{
			TArray<FInventoryEntry> Needed;
			for (const FRecipeIngredient& In : Recipe->Inputs)
			{
				FInventoryEntry Entry;
				Entry.Item = In.Item;
				// Reserve inputs for everything already waiting in the queue.
				Entry.Count = In.Count * (1 + Queued);
				Needed.Add(Entry);
			}
			if (Inventory->HasItems(Needed) && Crafting->Enqueue(Recipe))
			{
				Queued++;
				bQueuedAny = true;
				break;
			}
		}
		if (!bQueuedAny)
		{
			break;
		}
	}

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(2, 4.f, FColor::Green,
			FString::Printf(TEXT("%s: +%d in, %d queued, %d out"),
				*GetNameSafe(Def), Deposited, Queued, Withdrawn));
	}
	return true;
}

void AMachinePiece::OnInteractLocal_Implementation(AActor* Interactor)
{
	OpenMachineUI(Cast<APawn>(Interactor));
}

FGameplayTagContainer AMachinePiece::GetInteractionTags_Implementation() const
{
	FGameplayTagContainer InteractionTags;
	InteractionTags.AddTag(ExoneerTags::Interaction_OpenContainer);
	InteractionTags.AddTag(ExoneerTags::Interaction_Use);
	return InteractionTags;
}

void AMachinePiece::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AMachinePiece, MachineState);
}
