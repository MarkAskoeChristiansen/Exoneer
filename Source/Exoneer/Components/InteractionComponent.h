// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "InteractionComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnFocusChanged, AActor*, FocusedActor);

/**
 * Player-facing trace component that finds IInteractable actors in front of
 * the camera and lets the player Interact with them.
 */
UCLASS(ClassGroup = (Exoneer), meta = (BlueprintSpawnableComponent))
class EXONEER_API UInteractionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UInteractionComponent();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Interaction") float TraceDistance = 350.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Interaction") float TraceRadius = 8.f;
	UPROPERTY(BlueprintAssignable) FOnFocusChanged OnFocusChanged;

	UFUNCTION(BlueprintPure, Category = "Interaction") AActor* GetFocusedActor() const { return FocusedActor.Get(); }

	UFUNCTION(BlueprintCallable, Category = "Interaction") bool TryInteract();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn) override;

protected:
	TWeakObjectPtr<AActor> FocusedActor;
	bool TraceForward(FHitResult& OutHit) const;
};
