// Copyright Exoneer contributors.
#include "Machines/OxygenGeneratorPiece.h"
#include "Components/CraftingComponent.h"
#include "Components/OxygenComponent.h"
#include "Components/PowerComponent.h"
#include "Components/InventoryComponent.h"
#include "Components/ConstructionComponent.h"
#include "Data/PieceDefinitionDataAsset.h"

AOxygenGeneratorPiece::AOxygenGeneratorPiece()
{
	// Server-side production/state cadence; clients receive state via rep.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.25f;

	Crafting = CreateDefaultSubobject<UCraftingComponent>(TEXT("Crafting"));
	Crafting->StationType = EExoneerRecipeStation::OxygenGenerator;
	Crafting->InputInventory = Inventory;
	Crafting->OutputInventory = Inventory;
	Crafting->PowerSource = Power;

	Oxygen = CreateDefaultSubobject<UOxygenComponent>(TEXT("Oxygen"));
	Oxygen->Capacity = 200.f;   // reservoir size; production rate comes from Def
}

void AOxygenGeneratorPiece::ApplyDefinitionStats()
{
	Super::ApplyDefinitionStats();
	if (Def && Oxygen)
	{
		Oxygen->ProductionPerSec = Def->OxygenProductionPerSec;
	}
}

void AOxygenGeneratorPiece::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority())
	{
		return;
	}
	if (Construction && !Construction->IsComplete())
	{
		UpdateMachineState(EMachineState::Idle);
		return;
	}

	// Passive production into the reservoir while fully powered. Item-form
	// oxygen (tank refills from ice) runs through the crafting queue instead.
	if (Oxygen && Power && Power->IsPowered())
	{
		Oxygen->Deposit(Oxygen->ProductionPerSec * DeltaSeconds);
	}

	if (Crafting)
	{
		UpdateMachineState(Crafting->ComputeMachineState());
	}
}
