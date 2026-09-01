// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "InteractionComponent.generated.h"

class UCraftingComponent;
class URecipeDefinitionDataAsset;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnFocusChanged, AActor*, FocusedActor);

/**
 * Player-facing interaction pipeline.
 *
 * Focus (trace + highlight + prompt) is purely local and ticks only for the
 * locally controlled pawn. Interacting is server-authoritative:
 * TryInteract executes directly on authority (listen host) or sends
 * Server_TryInteract; the server revalidates range and interface, runs
 * OnInteract, then Client_InteractSucceeded triggers OnInteractLocal on the
 * initiating client (UI opening, FX).
 *
 * This component also carries the player's machine-UI intent RPCs
 * (recipe queueing), because client RPCs must originate from a
 * connection-owned actor and machines are not connection-owned.
 */
UCLASS(ClassGroup = (Exoneer), meta = (BlueprintSpawnableComponent))
class EXONEER_API UInteractionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UInteractionComponent();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Interaction") float TraceDistance = 350.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Interaction") float TraceRadius = 8.f;

	/** Server range check factor on top of TraceDistance (latency slack). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Interaction") float ServerRangeSlack = 1.5f;

	UPROPERTY(BlueprintAssignable) FOnFocusChanged OnFocusChanged;

	UFUNCTION(BlueprintPure, Category = "Interaction") AActor* GetFocusedActor() const { return FocusedActor.Get(); }

	/** Interaction verb tags of the focused actor, for the HUD. */
	UFUNCTION(BlueprintPure, Category = "Interaction") FGameplayTagContainer GetFocusedInteractionTags() const;

	/** Begin an interaction with the focused actor. Safe to call on any machine. */
	UFUNCTION(BlueprintCallable, Category = "Interaction") bool TryInteract();

	// --- Machine UI intents (client -> server via this connection-owned component) ---

	UFUNCTION(BlueprintCallable, Category = "Interaction") void RequestEnqueueRecipe(UCraftingComponent* Crafting, URecipeDefinitionDataAsset* Recipe);
	UFUNCTION(BlueprintCallable, Category = "Interaction") void RequestClearQueue(UCraftingComponent* Crafting);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn) override;

protected:
	TWeakObjectPtr<AActor> FocusedActor;

	bool TraceForward(FHitResult& OutHit) const;

	/** SERVER. Range + interface validation shared by all intent RPCs. */
	bool ServerValidateReach(const AActor* Target) const;

	/** SERVER. Execute the authoritative interaction and notify the client. */
	void ExecuteInteract(AActor* Target);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_TryInteract(AActor* Target);

	UFUNCTION(Client, Reliable)
	void Client_InteractSucceeded(AActor* Target);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_EnqueueRecipe(UCraftingComponent* Crafting, URecipeDefinitionDataAsset* Recipe);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_ClearQueue(UCraftingComponent* Crafting);
};
