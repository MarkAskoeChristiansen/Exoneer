// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "FirstPersonEngineerController.generated.h"

/**
 * Thin player controller — most input is handled by the pawn via Enhanced
 * Input. This subclass exists so designers can override HUD class, click
 * handling, etc., from Blueprints.
 */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API AFirstPersonEngineerController : public APlayerController
{
	GENERATED_BODY()
public:
	AFirstPersonEngineerController();
	virtual void BeginPlay() override;
};
