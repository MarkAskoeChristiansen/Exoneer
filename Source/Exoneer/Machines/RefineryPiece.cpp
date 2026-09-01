// Copyright Exoneer contributors.
#include "Machines/RefineryPiece.h"
#include "Components/CraftingComponent.h"
#include "Components/PowerComponent.h"
#include "Components/InventoryComponent.h"
#include "Components/ConstructionComponent.h"

ARefineryPiece::ARefineryPiece()
{
	// Server-side state refresh cadence; clients receive MachineState via rep.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.25f;

	Crafting = CreateDefaultSubobject<UCraftingComponent>(TEXT("Crafting"));
	Crafting->StationType = EExoneerRecipeStation::Refinery;
	Crafting->InputInventory = Inventory;
	Crafting->OutputInventory = Inventory;
	Crafting->PowerSource = Power;
}

void ARefineryPiece::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority() || !Crafting)
	{
		return;
	}
	if (Construction && !Construction->IsComplete())
	{
		UpdateMachineState(EMachineState::Idle);
		return;
	}
	UpdateMachineState(Crafting->ComputeMachineState());
}
