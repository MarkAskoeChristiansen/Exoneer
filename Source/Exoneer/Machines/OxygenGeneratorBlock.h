// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Machines/MachineBlock.h"
#include "OxygenGeneratorBlock.generated.h"

class UOxygenComponent;
class UCraftingComponent;

/**
 * Converts ice items into stored oxygen at OxygenComponent::ProductionPerSec
 * while it has power. Players refuel their suit by interacting with this
 * block (or via an oxygen tank linked through the same grid).
 */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API AOxygenGeneratorBlock : public AMachineBlock
{
	GENERATED_BODY()
public:
	AOxygenGeneratorBlock();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) UOxygenComponent* Oxygen = nullptr;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) UCraftingComponent* Crafting = nullptr;

	virtual void OnPlaced_Implementation(const FIntVector& Cell) override;
	virtual void Tick(float DeltaSeconds) override;
};
