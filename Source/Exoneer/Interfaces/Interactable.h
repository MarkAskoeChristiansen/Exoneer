// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "GameplayTagContainer.h"
#include "Interactable.generated.h"

UINTERFACE(BlueprintType, MinimalAPI)
class UInteractable : public UInterface { GENERATED_BODY() };

/**
 * Anything the player can highlight and interact with via the InteractionComponent.
 *
 * Network split:
 *  - OnFocusGained / OnFocusLost run only on the focusing player's machine (cosmetic).
 *  - OnInteract runs ON THE SERVER and is the authoritative gameplay effect.
 *  - OnInteractLocal runs on the INTERACTING CLIENT after the server confirmed the
 *    interaction (open UI, play FX). Never mutate gameplay state here.
 */
class EXONEER_API IInteractable
{
	GENERATED_BODY()

public:
	/** Called when the interaction beam first hits this actor. Local, cosmetic. */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Interact")
	void OnFocusGained(AActor* Interactor);
	virtual void OnFocusGained_Implementation(AActor* Interactor) {}

	/** Called when the interaction beam moves away. Local, cosmetic. */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Interact")
	void OnFocusLost(AActor* Interactor);
	virtual void OnFocusLost_Implementation(AActor* Interactor) {}

	/** Authoritative interaction. Runs on the server. Return true if it did something. */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Interact")
	bool OnInteract(AActor* Interactor);
	virtual bool OnInteract_Implementation(AActor* Interactor) { return false; }

	/** Client-side follow-up on the interacting player's machine (UI, FX). */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Interact")
	void OnInteractLocal(AActor* Interactor);
	virtual void OnInteractLocal_Implementation(AActor* Interactor) {}

	/** Localized prompt displayed on the HUD ("Open", "Mine", "Pilot", ...). */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Interact")
	FText GetInteractionPrompt() const;
	virtual FText GetInteractionPrompt_Implementation() const { return FText::FromString(TEXT("Interact")); }

	/** Interaction verb tags (ExoneerTags::Interaction_*) for HUD icon/verb filtering. */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Interact")
	FGameplayTagContainer GetInteractionTags() const;
	virtual FGameplayTagContainer GetInteractionTags_Implementation() const { return FGameplayTagContainer(); }
};
