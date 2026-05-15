// Copyright Exoneer contributors.
#include "Building/BuildableBlock.h"
#include "Building/BlockGridActor.h"
#include "Data/BlockDefinitionDataAsset.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"

ABuildableBlock::ABuildableBlock()
{
	PrimaryActorTick.bCanEverTick = false;
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	SetRootComponent(Mesh);
	Mesh->SetCollisionProfileName(TEXT("BlockAll"));
	Mesh->SetMobility(EComponentMobility::Movable);
}

void ABuildableBlock::Initialize(ABlockGridActor* Grid, UBlockDefinitionDataAsset* InDef, FIntVector Cell, int32 InRotation)
{
	OwningGrid = Grid;
	Definition = InDef;
	GridCoord = Cell;
	RotationStep = InRotation;
	Health = Definition ? Definition->MaxHealth : 100.f;

	if (Definition && Mesh)
	{
		if (UStaticMesh* M = Definition->PreviewMesh.LoadSynchronous())
		{
			Mesh->SetStaticMesh(M);
		}
	}
}

void ABuildableBlock::OnPlaced_Implementation(const FIntVector& Cell)
{
	GridCoord = Cell;
}

void ABuildableBlock::OnRemoved_Implementation()
{
	// Subclasses (machines, batteries, ...) override to disconnect from networks.
}

void ABuildableBlock::Repair(float Amount)
{
	const float Max = GetMaxHealth_Implementation();
	Health = FMath::Clamp(Health + Amount, 0.f, Max);
}

FText ABuildableBlock::GetInteractionPrompt_Implementation() const
{
	if (Definition) return Definition->DisplayName;
	return FText::FromString(TEXT("Block"));
}

float ABuildableBlock::ApplyExoneerDamage_Implementation(float Amount, EExoneerDamageType Type, AActor* Instigator)
{
	const float Old = Health;
	Health = FMath::Clamp(Health - Amount, 0.f, GetMaxHealth_Implementation());
	const float Dealt = Old - Health;
	if (Health <= 0.f && OwningGrid)
	{
		OwningGrid->RemoveBlockAt(GridCoord);
	}
	return Dealt;
}

float ABuildableBlock::GetMaxHealth_Implementation() const
{
	return Definition ? Definition->MaxHealth : 100.f;
}
