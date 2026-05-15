// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Machines/MachineBlock.h"
#include "RefineryBlock.generated.h"

class UCraftingComponent;

/** Converts raw resources (stone, ore, ice) into refined materials. */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API ARefineryBlock : public AMachineBlock
{
	GENERATED_BODY()
public:
	ARefineryBlock();
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) UCraftingComponent* Crafting = nullptr;
};
