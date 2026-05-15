// Copyright Exoneer contributors.
#include "Machines/RefineryBlock.h"
#include "Components/CraftingComponent.h"
#include "Components/PowerComponent.h"
#include "Components/InventoryComponent.h"

ARefineryBlock::ARefineryBlock()
{
	Crafting = CreateDefaultSubobject<UCraftingComponent>(TEXT("Crafting"));
	Crafting->StationType = EExoneerRecipeStation::Refinery;
	Crafting->InputInventory = Inventory;
	Crafting->OutputInventory = Inventory;
	Crafting->PowerSource = Power;
}
