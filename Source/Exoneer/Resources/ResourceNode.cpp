// Copyright Exoneer contributors.
#include "Resources/ResourceNode.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InventoryComponent.h"
#include "Data/ItemDefinitionDataAsset.h"
#include "Net/UnrealNetwork.h"

AResourceNode::AResourceNode()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	SetRootComponent(Mesh);
	Mesh->SetCollisionProfileName(TEXT("BlockAll"));
}

void AResourceNode::BeginPlay()
{
	Super::BeginPlay();
	// Integrity feedback scales RELATIVE to how the node was authored - a
	// 2.2x deposit must not pop down to 1x on the first mining hit.
	if (Mesh)
	{
		BaseMeshScale = Mesh->GetComponentScale();
	}
	// Server owns Integrity; clients may already hold a replicated value.
	if (HasAuthority())
	{
		Integrity = MaxIntegrity;
	}
}

void AResourceNode::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AResourceNode, Integrity);
}

void AResourceNode::MineByPlayer(float DamageAmount, UInventoryComponent* Inventory)
{
	if (!HasAuthority())
	{
		UE_LOG(LogTemp, Warning, TEXT("AResourceNode::MineByPlayer is server-only; route intent through UMiningToolComponent::Server_MineTarget."));
		return;
	}
	if (DamageAmount <= 0.f || Integrity <= 0.f) return;
	Integrity = FMath::Max(0.f, Integrity - DamageAmount);
	DamageAccumulator += DamageAmount;

	// Convert accumulated damage into yield units, scaled by deposit richness.
	if (DamagePerYieldUnit > 0.f && Inventory)
	{
		UItemDefinitionDataAsset* I = Yield.LoadSynchronous();
		const int32 CountPerUnit = FMath::Max(1, FMath::RoundToInt(YieldPerHit * YieldMultiplier));
		while (DamageAccumulator >= DamagePerYieldUnit)
		{
			DamageAccumulator -= DamagePerYieldUnit;
			if (I)
			{
				Inventory->AddItem(I, CountPerUnit);
			}
		}
	}

	// The listen host shares the client feedback path; clients get OnRep.
	ApplyIntegrityVisuals();

	if (Integrity <= 0.f)
	{
		// The unreliable depletion multicast races actor teardown if we destroy
		// in the same frame, so hide the node and let a short lifespan reap it
		// after the RPC and the final Integrity replication have gone out.
		Multicast_OnDepleted();
		SetActorEnableCollision(false);
		SetActorHiddenInGame(true);
		SetLifeSpan(2.f);
	}
}

void AResourceNode::OnRep_Integrity()
{
	ApplyIntegrityVisuals();
}

void AResourceNode::ApplyIntegrityVisuals()
{
	// Visual feedback hook: scale the mesh down a bit as integrity falls.
	if (Mesh)
	{
		const float Frac = FMath::Clamp(GetIntegrityFraction(), 0.f, 1.f);
		Mesh->SetWorldScale3D(BaseMeshScale * FMath::Lerp(0.5f, 1.f, Frac));
	}
}

void AResourceNode::Multicast_OnDepleted_Implementation()
{
	OnDepletedBP();
}
