// Copyright Exoneer contributors.
#include "Machines/SolarPanelPiece.h"
#include "Components/PowerComponent.h"
#include "Components/ConstructionComponent.h"
#include "Data/PieceDefinitionDataAsset.h"
#include "World/PlanetEnvironmentManager.h"
#include "EngineUtils.h"

ASolarPanelPiece::ASolarPanelPiece()
{
	// Sun exposure changes slowly; a coarse server tick is plenty.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.5f;
}

void ASolarPanelPiece::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority())
	{
		for (TActorIterator<APlanetEnvironmentManager> It(GetWorld()); It; ++It)
		{
			Environment = *It;
			break;
		}
	}
}

void ASolarPanelPiece::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority() || !Power || !Def)
	{
		return;
	}
	if (Construction && !Construction->IsComplete())
	{
		return;   // inert until built; Power->bEnabled is false anyway
	}

	const float Sun = Environment.IsValid() ? FMath::Clamp(Environment->GetSunFraction(), 0.f, 1.f) : 1.f;
	Power->NominalOutput = FMath::Max(0.f, Def->PowerDelta) * Sun;
}
