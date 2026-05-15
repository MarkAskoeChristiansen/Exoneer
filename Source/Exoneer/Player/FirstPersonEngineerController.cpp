// Copyright Exoneer contributors.
#include "Player/FirstPersonEngineerController.h"

AFirstPersonEngineerController::AFirstPersonEngineerController()
{
	bShowMouseCursor = false;
	DefaultMouseCursor = EMouseCursor::Default;
}

void AFirstPersonEngineerController::BeginPlay()
{
	Super::BeginPlay();
	FInputModeGameOnly Mode;
	SetInputMode(Mode);
}
