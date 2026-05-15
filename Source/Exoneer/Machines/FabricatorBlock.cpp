// Copyright Exoneer contributors.
#include "Machines/FabricatorBlock.h"
#include "Components/CraftingComponent.h"
#include "Components/PowerComponent.h"
#include "Components/InventoryComponent.h"

AFabricatorBlock::AFabricatorBlock()
{
	Crafting = CreateDefaultSubobject<UCraftingComponent>(TEXT("Crafting"));
	Crafting->StationType = EExoneerRecipeStation::Fabricator;
	Crafting->InputInventory = Inventory;
	Crafting->OutputInventory = Inventory;
	Crafting->PowerSource = Power;
}
