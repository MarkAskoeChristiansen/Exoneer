// Copyright Exoneer contributors.
#include "Components/InteractionComponent.h"
#include "Interfaces/Interactable.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/Actor.h"

UInteractionComponent::UInteractionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.05f;
}

bool UInteractionComponent::TraceForward(FHitResult& OutHit) const
{
	AActor* Owner = GetOwner();
	if (!Owner) return false;

	UCameraComponent* Cam = Owner->FindComponentByClass<UCameraComponent>();
	const FVector Start = Cam ? Cam->GetComponentLocation() : Owner->GetActorLocation();
	const FVector Dir = Cam ? Cam->GetForwardVector() : Owner->GetActorForwardVector();
	const FVector End = Start + Dir * TraceDistance;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ExoneerInteract), false, Owner);
	return GetWorld()->SweepSingleByChannel(
		OutHit, Start, End, FQuat::Identity, ECC_Visibility,
		FCollisionShape::MakeSphere(TraceRadius), Params);
}

void UInteractionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn)
{
	Super::TickComponent(DeltaTime, TickType, TickFn);

	AActor* NewFocus = nullptr;
	FHitResult Hit;
	if (TraceForward(Hit))
	{
		AActor* Hot = Hit.GetActor();
		if (Hot && Hot->Implements<UInteractable>())
		{
			NewFocus = Hot;
		}
	}

	AActor* Old = FocusedActor.Get();
	if (Old != NewFocus)
	{
		if (Old && Old->Implements<UInteractable>())
		{
			IInteractable::Execute_OnFocusLost(Old, GetOwner());
		}
		FocusedActor = NewFocus;
		if (NewFocus)
		{
			IInteractable::Execute_OnFocusGained(NewFocus, GetOwner());
		}
		OnFocusChanged.Broadcast(NewFocus);
	}
}

bool UInteractionComponent::TryInteract()
{
	AActor* Focus = FocusedActor.Get();
	if (Focus && Focus->Implements<UInteractable>())
	{
		return IInteractable::Execute_OnInteract(Focus, GetOwner());
	}
	return false;
}
