// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "ExoneerTypes.h"
#include "ExoneerGameState.generated.h"

/**
 * Replicated sandbox session state: optional projects and Handshake knowledge.
 * World subsystems do not replicate; this is the channel.
 */
UCLASS()
class EXONEER_API AExoneerGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	AExoneerGameState();

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Project")
	TArray<FProjectRuntime> Projects;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Project")
	FOrbitalKnowledge Orbital;

	/** Whole sols elapsed on the server clock (TimeOfDay wraps). */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "World")
	int32 SolIndex = 0;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
};
