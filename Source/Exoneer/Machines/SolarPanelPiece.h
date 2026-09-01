// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Machines/MachinePiece.h"
#include "SolarPanelPiece.generated.h"

class APlanetEnvironmentManager;

/**
 * Produces power proportional to the planet's current sun fraction:
 * NominalOutput = max(0, Def->PowerDelta) * sun fraction, rescaled from the
 * definition each server tick. With no environment manager in the level the
 * sun fraction defaults to 1.
 */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API ASolarPanelPiece : public AMachinePiece
{
	GENERATED_BODY()

public:
	ASolarPanelPiece();

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

	/** Cached environment manager (found once via actor iteration). */
	TWeakObjectPtr<APlanetEnvironmentManager> Environment;
};
