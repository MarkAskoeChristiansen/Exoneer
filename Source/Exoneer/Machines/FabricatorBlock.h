// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Machines/MachineBlock.h"
#include "FabricatorBlock.generated.h"

class UCraftingComponent;

/** Crafts components and block prefabs from refined materials. */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API AFabricatorBlock : public AMachineBlock
{
	GENERATED_BODY()
public:
	AFabricatorBlock();
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) UCraftingComponent* Crafting = nullptr;
};
