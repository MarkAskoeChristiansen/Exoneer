// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Machines/MachineBlock.h"
#include "SolarPanelBlock.generated.h"

class APlanetEnvironmentManager;

/** Produces power proportional to the planet's current sun exposure. */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API ASolarPanelBlock : public AMachineBlock
{
	GENERATED_BODY()
public:
	ASolarPanelBlock();
	virtual void Tick(float DeltaSeconds) override;
};
