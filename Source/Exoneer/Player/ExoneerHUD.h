// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "ExoneerTypes.h"
#include "ExoneerHUD.generated.h"

class APlayerSurvivalCharacter;

/**
 * Visor HUD v1: monochrome vector-line instrumentation drawn straight on the
 * canvas (GAME-SCOPE.md module 8). This is the prototype form of the helmet
 * visor - no widgets, no assets, pure code - and replaces the temporary
 * on-screen debug messages. The wrist computer and physical dashboards build
 * on top of this later.
 */
UCLASS()
class EXONEER_API AExoneerHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;
	virtual void Tick(float DeltaSeconds) override;

protected:
	/** Smoothed suit power drain (units/s) for the estimated-time-to-empty readout. */
	float SmoothedSuitDrain = 0.f;
	float LastSuitPower = -1.f;

	void DrawCrosshair();
	void DrawVitals(const APlayerSurvivalCharacter* Engineer);
	void DrawToolPanel(APlayerSurvivalCharacter* Engineer);
	void DrawInteractionPrompt(const APlayerSurvivalCharacter* Engineer);
	void DrawInventory(const APlayerSurvivalCharacter* Engineer);

	/** Visor line helper: label plus value with a thin underline. */
	void DrawReadout(float X, float& Y, const FString& Label, const FString& Value, const FLinearColor& Color);
};
