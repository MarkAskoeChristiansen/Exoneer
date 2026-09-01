// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Machines/MachinePiece.h"
#include "FabricatorPiece.generated.h"

class UCraftingComponent;

/** Crafts components and piece/block prefabs from refined materials. */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API AFabricatorPiece : public AMachinePiece
{
	GENERATED_BODY()

public:
	AFabricatorPiece();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Machine")
	TObjectPtr<UCraftingComponent> Crafting;

	virtual void Tick(float DeltaSeconds) override;
};
