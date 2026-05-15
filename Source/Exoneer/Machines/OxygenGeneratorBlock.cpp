// Copyright Exoneer contributors.
#include "Machines/OxygenGeneratorBlock.h"
#include "Components/OxygenComponent.h"
#include "Components/CraftingComponent.h"
#include "Components/PowerComponent.h"
#include "Components/InventoryComponent.h"
#include "Data/BlockDefinitionDataAsset.h"

AOxygenGeneratorBlock::AOxygenGeneratorBlock()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.25f;
	Oxygen = CreateDefaultSubobject<UOxygenComponent>(TEXT("Oxygen"));
	Crafting = CreateDefaultSubobject<UCraftingComponent>(TEXT("Crafting"));
	Crafting->StationType = EExoneerRecipeStation::OxygenGenerator;
	Crafting->InputInventory = Inventory;
	Crafting->OutputInventory = Inventory;
	Crafting->PowerSource = Power;
}

void AOxygenGeneratorBlock::OnPlaced_Implementation(const FIntVector& Cell)
{
	Super::OnPlaced_Implementation(Cell);
	if (Definition && Oxygen)
	{
		Oxygen->Capacity = Definition->FuelCapacity > 0.f ? Definition->FuelCapacity : 200.f;
		Oxygen->ProductionPerSec = Definition->OxygenProduction;
	}
}

void AOxygenGeneratorBlock::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Oxygen || !Power) return;
	// Produce oxygen passively while powered. Real "convert ice" production
	// goes through the Crafting queue (recipe: 1 ice → N oxygen).
	if (Power->IsPowered())
	{
		Oxygen->Deposit(Oxygen->ProductionPerSec * DeltaSeconds);
	}
}
