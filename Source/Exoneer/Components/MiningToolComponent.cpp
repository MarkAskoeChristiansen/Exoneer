// Copyright Exoneer contributors.
#include "Components/MiningToolComponent.h"
#include "Components/InventoryComponent.h"
#include "Components/SurvivalStatsComponent.h"
#include "Resources/ResourceNode.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/Actor.h"

UMiningToolComponent::UMiningToolComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UMiningToolComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn)
{
	Super::TickComponent(DeltaTime, TickType, TickFn);
	if (!bActive) return;

	AActor* Owner = GetOwner();
	if (!Owner) return;

	// Need suit power to mine.
	if (USurvivalStatsComponent* Stats = Owner->FindComponentByClass<USurvivalStatsComponent>())
	{
		if (Stats->SuitPower <= 0.f) return;
		Stats->AddSuitPower(-SuitPowerDrainPerSec * DeltaTime);
	}

	UCameraComponent* Cam = Owner->FindComponentByClass<UCameraComponent>();
	const FVector Start = Cam ? Cam->GetComponentLocation() : Owner->GetActorLocation();
	const FVector Dir = Cam ? Cam->GetForwardVector() : Owner->GetActorForwardVector();
	const FVector End = Start + Dir * Range;

	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ExoneerMine), false, Owner);
	if (GetWorld()->SweepSingleByChannel(Hit, Start, End, FQuat::Identity, ECC_Visibility,
		FCollisionShape::MakeSphere(Radius), Params))
	{
		if (AResourceNode* Node = Cast<AResourceNode>(Hit.GetActor()))
		{
			UInventoryComponent* Inv = Owner->FindComponentByClass<UInventoryComponent>();
			Node->MineByPlayer(DamagePerSec * DeltaTime, Inv);
			OnMiningProgress.Broadcast(Node->GetIntegrityFraction());
		}
	}
}
