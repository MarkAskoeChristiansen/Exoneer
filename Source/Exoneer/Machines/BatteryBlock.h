// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Machines/MachineBlock.h"
#include "BatteryBlock.generated.h"

/** A battery: stores energy and contributes to the grid's power network. */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API ABatteryBlock : public AMachineBlock
{
	GENERATED_BODY()
public:
	ABatteryBlock();
	virtual void OnPlaced_Implementation(const FIntVector& Cell) override;
};
