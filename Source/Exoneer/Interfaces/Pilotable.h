// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "Pilotable.generated.h"

UINTERFACE(BlueprintType, MinimalAPI)
class UPilotable : public UInterface { GENERATED_BODY() };

class APawn;

class EXONEER_API IPilotable
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Vehicle")
	bool EnterPilot(APawn* Pilot);
	virtual bool EnterPilot_Implementation(APawn* Pilot) { return false; }

	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Vehicle")
	void ExitPilot(APawn* Pilot);
	virtual void ExitPilot_Implementation(APawn* Pilot) {}

	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Vehicle")
	void ApplyPilotInput(const FVector& MoveInput, const FVector& RotateInput);
	virtual void ApplyPilotInput_Implementation(const FVector& MoveInput, const FVector& RotateInput) {}
};
