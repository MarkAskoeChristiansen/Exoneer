// Copyright Exoneer contributors.
#include "Resources/ResourceNode.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InventoryComponent.h"
#include "Data/ItemDefinitionDataAsset.h"

AResourceNode::AResourceNode()
{
	PrimaryActorTick.bCanEverTick = false;
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	SetRootComponent(Mesh);
	Mesh->SetCollisionProfileName(TEXT("BlockAll"));
}

void AResourceNode::BeginPlay()
{
	Super::BeginPlay();
	Integrity = MaxIntegrity;
}

void AResourceNode::MineByPlayer(float DamageAmount, UInventoryComponent* Inventory)
{
	if (DamageAmount <= 0.f || Integrity <= 0.f) return;
	Integrity = FMath::Max(0.f, Integrity - DamageAmount);
	DamageAccumulator += DamageAmount;

	// Convert accumulated damage into yield units.
	if (DamagePerYieldUnit > 0.f && Inventory)
	{
		UItemDefinitionDataAsset* I = Yield.LoadSynchronous();
		while (DamageAccumulator >= DamagePerYieldUnit)
		{
			DamageAccumulator -= DamagePerYieldUnit;
			if (I)
			{
				Inventory->AddItem(I, FMath::Max(1, YieldPerHit));
			}
		}
	}

	// Visual feedback hook: scale the mesh down a bit as integrity falls.
	if (Mesh)
	{
		const float Frac = GetIntegrityFraction();
		Mesh->SetWorldScale3D(FVector(FMath::Lerp(0.5f, 1.f, Frac)));
	}

	if (Integrity <= 0.f) Destroy();
}
