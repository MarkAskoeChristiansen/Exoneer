// Copyright Exoneer contributors.
#include "Machines/SolarPanelBlock.h"
#include "Components/PowerComponent.h"
#include "Data/BlockDefinitionDataAsset.h"
#include "World/PlanetEnvironmentManager.h"
#include "EngineUtils.h"

ASolarPanelBlock::ASolarPanelBlock()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.5f;
}

void ASolarPanelBlock::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Power || !Definition) return;

	float SunFraction = 1.f;
	for (TActorIterator<APlanetEnvironmentManager> It(GetWorld()); It; ++It)
	{
		SunFraction = It->GetSunExposureFraction();
		break;
	}
	Power->NominalOutput = FMath::Max(0.f, Definition->PowerDelta) * SunFraction;
}
