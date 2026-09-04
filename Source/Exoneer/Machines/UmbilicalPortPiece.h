// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Machines/MachinePiece.h"
#include "UmbilicalPortPiece.generated.h"

class APlayerSurvivalCharacter;

/** What E will do on this port for a given pawn. The prompt always matches. */
enum class EUmbilicalVerb : uint8
{
	Connect,
	Disconnect,
	PatchSeal,
	ReplaceSeal,
	None
};

/**
 * The only interactable that connects a suit. Batteries and oxygen generators
 * keep their machine-exchange E. Power comes from the structure network
 * (this piece is a 600 W consumer while charging); O2 is withdrawn from any
 * oxygen-generator reservoir on the same structure.
 */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API AUmbilicalPortPiece : public AMachinePiece
{
	GENERATED_BODY()

public:
	AUmbilicalPortPiece();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Umbilical")
	float PortPowerW = 600.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Umbilical")
	float PortO2Lps = 2.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Umbilical")
	float UmbilicalLengthCm = 800.f;

	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void ApplyDefinitionStats() override;

	virtual bool OnInteract_Implementation(AActor* Interactor) override;
	virtual void OnInteractLocal_Implementation(AActor* Interactor) override;
	virtual FText GetInteractionPrompt_Implementation() const override;
	virtual FGameplayTagContainer GetInteractionTags_Implementation() const override;

	EUmbilicalVerb ResolveVerb(const APlayerSurvivalCharacter* User) const;
	FText GetPromptFor(const APlayerSurvivalCharacter* User) const;

	/** SERVER. Clear the weak pointer when the pawn drops the link. */
	void NotifyPawnDisconnected(APlayerSurvivalCharacter* Pawn);

	APlayerSurvivalCharacter* GetConnectedPawn() const { return ConnectedPawn.Get(); }

protected:
	TWeakObjectPtr<APlayerSurvivalCharacter> ConnectedPawn;

	void DisconnectConnected(bool bRangeBreak);
	void TransferToConnected(float DeltaSeconds);
};
