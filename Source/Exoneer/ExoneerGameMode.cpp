// Copyright Exoneer contributors.
#include "ExoneerGameMode.h"
#include "Player/PlayerSurvivalCharacter.h"
#include "Player/FirstPersonEngineerController.h"
#include "Player/ExoneerHUD.h"
#include "World/ExoneerGameState.h"

AExoneerGameMode::AExoneerGameMode()
{
	// Defaults — Blueprints under /Game/Exoneer/Blueprints/ should subclass
	// these and be selected as the active GameMode in the level.
	DefaultPawnClass = APlayerSurvivalCharacter::StaticClass();
	PlayerControllerClass = AFirstPersonEngineerController::StaticClass();
	HUDClass = AExoneerHUD::StaticClass();
	GameStateClass = AExoneerGameState::StaticClass();
}
