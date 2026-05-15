// Copyright Exoneer contributors.
#include "Machines/MachineBlock.h"
#include "Components/PowerComponent.h"
#include "Components/InventoryComponent.h"
#include "Components/ConveyorComponent.h"
#include "Data/BlockDefinitionDataAsset.h"
#include "GameFramework/Pawn.h"

AMachineBlock::AMachineBlock()
{
	Power     = CreateDefaultSubobject<UPowerComponent>(TEXT("Power"));
	Inventory = CreateDefaultSubobject<UInventoryComponent>(TEXT("Inventory"));
	Inventory->bUseWeight = false;
	Conveyor  = CreateDefaultSubobject<UConveyorComponent>(TEXT("Conveyor"));
}

void AMachineBlock::OnPlaced_Implementation(const FIntVector& Cell)
{
	Super::OnPlaced_Implementation(Cell);
	if (Definition)
	{
		if (Definition->PowerDelta < 0.f)
		{
			Power->NominalDraw = -Definition->PowerDelta;
		}
		else if (Definition->PowerDelta > 0.f)
		{
			Power->NominalOutput = Definition->PowerDelta;
		}
		Inventory->MaxCapacity = Definition->InventoryCapacity;
	}
}

bool AMachineBlock::OnInteract_Implementation(AActor* Interactor)
{
	return OpenMachineUI(Cast<APawn>(Interactor));
}
