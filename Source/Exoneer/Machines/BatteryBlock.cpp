// Copyright Exoneer contributors.
#include "Machines/BatteryBlock.h"
#include "Components/PowerComponent.h"
#include "Data/BlockDefinitionDataAsset.h"

ABatteryBlock::ABatteryBlock()
{
}

void ABatteryBlock::OnPlaced_Implementation(const FIntVector& Cell)
{
	Super::OnPlaced_Implementation(Cell);
	// Use FuelCapacity to mean "joules of storage" for batteries.
	if (Definition && Power)
	{
		Power->StorageCapacity = Definition->FuelCapacity > 0.f ? Definition->FuelCapacity : 50000.f;
		Power->StoredEnergy = 0.f;
	}
}
