// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "Interactable.generated.h"

UINTERFACE(BlueprintType, MinimalAPI)
class UInteractable : public UInterface { GENERATED_BODY() };

/**
 * Anything the player can highlight and interact with via the InteractionComponent.
 */
class EXONEER_API IInteractable
{
	GENERATED_BODY()

public:
	/** Called when the interaction beam first hits this actor. */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Interact")
	void OnFocusGained(AActor* Interactor);
	virtual void OnFocusGained_Implementation(AActor* Interactor) {}

	/** Called when the interaction beam moves away. */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Interact")
	void OnFocusLost(AActor* Interactor);
	virtual void OnFocusLost_Implementation(AActor* Interactor) {}

	/** Called when the player presses Interact. */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Interact")
	bool OnInteract(AActor* Interactor);
	virtual bool OnInteract_Implementation(AActor* Interactor) { return false; }

	/** Localized prompt displayed on the HUD ("Open", "Mine", "Pilot", ...). */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Interact")
	FText GetInteractionPrompt() const;
	virtual FText GetInteractionPrompt_Implementation() const { return FText::FromString(TEXT("Interact")); }
};
