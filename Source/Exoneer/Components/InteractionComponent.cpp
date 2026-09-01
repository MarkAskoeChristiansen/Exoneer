// Copyright Exoneer contributors.
#include "Components/InteractionComponent.h"
#include "Exoneer.h"
#include "Interfaces/Interactable.h"
#include "Components/CraftingComponent.h"
#include "Data/RecipeDefinitionDataAsset.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"

UInteractionComponent::UInteractionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.05f;
	// The intent RPCs (Server_TryInteract & friends) require a replicated component.
	SetIsReplicatedByDefault(true);
}

bool UInteractionComponent::TraceForward(FHitResult& OutHit) const
{
	AActor* Owner = GetOwner();
	if (!Owner || !GetWorld()) return false;

	const UCameraComponent* Cam = Owner->FindComponentByClass<UCameraComponent>();
	FVector Start;
	FVector Dir;
	if (Cam)
	{
		Start = Cam->GetComponentLocation();
		Dir = Cam->GetForwardVector();
	}
	else
	{
		FRotator ViewRotation;
		Owner->GetActorEyesViewPoint(Start, ViewRotation);
		Dir = ViewRotation.Vector();
	}
	const FVector End = Start + Dir * TraceDistance;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ExoneerInteract), false, Owner);
	return GetWorld()->SweepSingleByChannel(
		OutHit, Start, End, FQuat::Identity, ECC_Visibility,
		FCollisionShape::MakeSphere(TraceRadius), Params);
}

void UInteractionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn)
{
	Super::TickComponent(DeltaTime, TickType, TickFn);

	// Focus is purely cosmetic: trace only on the machine that controls the pawn.
	const APawn* Pawn = Cast<APawn>(GetOwner());
	if (!Pawn)
	{
		// Non-pawn owners never gain local control; stop ticking for good.
		SetComponentTickEnabled(false);
		return;
	}
	if (!Pawn->IsLocallyControlled())
	{
		return;
	}

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

FGameplayTagContainer UInteractionComponent::GetFocusedInteractionTags() const
{
	AActor* Focus = FocusedActor.Get();
	if (Focus && Focus->Implements<UInteractable>())
	{
		return IInteractable::Execute_GetInteractionTags(Focus);
	}
	return FGameplayTagContainer();
}

bool UInteractionComponent::TryInteract()
{
	AActor* Focus = FocusedActor.Get();
	if (!Focus || !Focus->Implements<UInteractable>())
	{
		return false;
	}

	const AActor* Owner = GetOwner();
	if (Owner && Owner->HasAuthority())
	{
		// Listen host: no RPC round trip, validate and execute in place.
		ExecuteInteract(Focus);
	}
	else
	{
		Server_TryInteract(Focus);
	}
	return true;
}

bool UInteractionComponent::ServerValidateReach(const AActor* Target) const
{
	if (!Target || !Target->Implements<UInteractable>())
	{
		return false;
	}

	const APawn* Pawn = Cast<APawn>(GetOwner());
	if (!Pawn)
	{
		return false;
	}

	FVector ViewLocation;
	FRotator ViewRotation;
	Pawn->GetActorEyesViewPoint(ViewLocation, ViewRotation);

	// Latency slack plus the target's bounding sphere, so large machines stay
	// usable when the client aimed at a far corner of their mesh.
	FVector Origin, Extent;
	Target->GetActorBounds(false, Origin, Extent);
	const float MaxReach = TraceDistance * ServerRangeSlack + Extent.Size();
	return FVector::Dist(ViewLocation, Origin) <= MaxReach;
}

void UInteractionComponent::ExecuteInteract(AActor* Target)
{
	const AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority())
	{
		return;
	}

	if (!ServerValidateReach(Target))
	{
		return;
	}

	// Authoritative gameplay effect first; cosmetics only if it did something.
	if (!IInteractable::Execute_OnInteract(Target, GetOwner()))
	{
		return;
	}

	const APawn* Pawn = Cast<APawn>(GetOwner());
	if (Pawn && Pawn->IsLocallyControlled())
	{
		// Listen host interacting with its own pawn: run the local path directly.
		IInteractable::Execute_OnInteractLocal(Target, GetOwner());
	}
	else
	{
		Client_InteractSucceeded(Target);
	}
}

bool UInteractionComponent::Server_TryInteract_Validate(AActor* Target)
{
	// Actor references can legitimately fail to resolve under relevancy;
	// a null target soft-fails in the implementation.
	return true;
}

void UInteractionComponent::Server_TryInteract_Implementation(AActor* Target)
{
	ExecuteInteract(Target);
}

void UInteractionComponent::Client_InteractSucceeded_Implementation(AActor* Target)
{
	if (Target && Target->Implements<UInteractable>())
	{
		IInteractable::Execute_OnInteractLocal(Target, GetOwner());
	}
}

// --- Machine UI intents -------------------------------------------------------

void UInteractionComponent::RequestEnqueueRecipe(UCraftingComponent* Crafting, URecipeDefinitionDataAsset* Recipe)
{
	if (!Crafting || !Recipe)
	{
		return;
	}

	const AActor* Owner = GetOwner();
	if (Owner && Owner->HasAuthority())
	{
		// Listen host: same validation path, no RPC.
		Server_EnqueueRecipe_Implementation(Crafting, Recipe);
	}
	else
	{
		Server_EnqueueRecipe(Crafting, Recipe);
	}
}

void UInteractionComponent::RequestClearQueue(UCraftingComponent* Crafting)
{
	if (!Crafting)
	{
		return;
	}

	const AActor* Owner = GetOwner();
	if (Owner && Owner->HasAuthority())
	{
		Server_ClearQueue_Implementation(Crafting);
	}
	else
	{
		Server_ClearQueue(Crafting);
	}
}

bool UInteractionComponent::Server_EnqueueRecipe_Validate(UCraftingComponent* Crafting, URecipeDefinitionDataAsset* Recipe)
{
	// Object references can fail to resolve; nulls soft-fail below.
	return true;
}

void UInteractionComponent::Server_EnqueueRecipe_Implementation(UCraftingComponent* Crafting, URecipeDefinitionDataAsset* Recipe)
{
	if (!Crafting || !Recipe)
	{
		return;
	}

	if (!ServerValidateReach(Crafting->GetOwner()))
	{
		return;
	}

	if (Recipe->Station != Crafting->StationType)
	{
		UE_LOG(LogExoneer, Warning, TEXT("Recipe %s rejected: wrong station type for %s."),
			*GetNameSafe(Recipe), *GetNameSafe(Crafting->GetOwner()));
		return;
	}

	Crafting->Enqueue(Recipe);
}

bool UInteractionComponent::Server_ClearQueue_Validate(UCraftingComponent* Crafting)
{
	return true;
}

void UInteractionComponent::Server_ClearQueue_Implementation(UCraftingComponent* Crafting)
{
	if (!Crafting)
	{
		return;
	}

	if (!ServerValidateReach(Crafting->GetOwner()))
	{
		return;
	}

	Crafting->ClearQueue();
}
