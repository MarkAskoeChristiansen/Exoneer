// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Machines/MachinePiece.h"
#include "RefineryPiece.generated.h"

class UCraftingComponent;

/** Converts raw resources (stone, ore, ice) into refined materials. */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API ARefineryPiece : public AMachinePiece
{
	GENERATED_BODY()

public:
	ARefineryPiece();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Machine")
	TObjectPtr<UCraftingComponent> Crafting;

	virtual void Tick(float DeltaSeconds) override;
};
