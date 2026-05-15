// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Components/InventoryComponent.h"
#include "CargoComponent.generated.h"

/**
 * Inventory specialized for storage blocks. Identical semantics to the base
 * InventoryComponent but provides a dedicated subclass for type-based queries
 * by the conveyor system.
 */
UCLASS(ClassGroup = (Exoneer), meta = (BlueprintSpawnableComponent))
class EXONEER_API UCargoComponent : public UInventoryComponent
{
	GENERATED_BODY()

public:
	UCargoComponent()
	{
		bUseWeight = false;     // Cargo blocks measure by volume
		MaxCapacity = 1000.f;
	}
};
