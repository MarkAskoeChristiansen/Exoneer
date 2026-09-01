// Copyright Exoneer contributors.
#include "Machines/FabricatorPiece.h"
#include "Components/CraftingComponent.h"
#include "Components/PowerComponent.h"
#include "Components/InventoryComponent.h"
#include "Components/ConstructionComponent.h"

AFabricatorPiece::AFabricatorPiece()
{
	// Server-side state refresh cadence; clients receive MachineState via rep.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.25f;

	Crafting = CreateDefaultSubobject<UCraftingComponent>(TEXT("Crafting"));
	Crafting->StationType = EExoneerRecipeStation::Fabricator;
	Crafting->InputInventory = Inventory;
	Crafting->OutputInventory = Inventory;
	Crafting->PowerSource = Power;
}

void AFabricatorPiece::Tick(float DeltaSeconds)
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
