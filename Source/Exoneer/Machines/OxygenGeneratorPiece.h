// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Machines/MachinePiece.h"
#include "OxygenGeneratorPiece.generated.h"

class UCraftingComponent;
class UOxygenComponent;

/**
 * Produces oxygen while powered: a passive baseline of
 * Def->OxygenProductionPerSec into its UOxygenComponent reservoir, plus
 * ice-to-oxygen-item conversion through the crafting queue. Suits refill
 * from this reservoir only through an umbilical port on the same structure.
 */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API AOxygenGeneratorPiece : public AMachinePiece
{
	GENERATED_BODY()

public:
	AOxygenGeneratorPiece();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Machine")
	TObjectPtr<UCraftingComponent> Crafting;

	/** Server-side oxygen reservoir suits tap into. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Machine")
	TObjectPtr<UOxygenComponent> Oxygen;

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void ApplyDefinitionStats() override;
};
