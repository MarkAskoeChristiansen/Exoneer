// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Building/BuildableBlock.h"
#include "Interfaces/Pilotable.h"
#include "CockpitBlock.generated.h"

class AVehicleGridActor;

/**
 * Pilot seat block. Forwards player input to the owning AVehicleGridActor
 * via the IPilotable interface (the vehicle grid handles thruster routing).
 */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API ACockpitBlock : public ABuildableBlock, public IPilotable
{
	GENERATED_BODY()
public:
	ACockpitBlock();

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly) APawn* CurrentPilot = nullptr;

	virtual bool EnterPilot_Implementation(APawn* Pilot) override;
	virtual void ExitPilot_Implementation(APawn* Pilot) override;
	virtual void ApplyPilotInput_Implementation(const FVector& MoveInput, const FVector& RotateInput) override;

	virtual bool OnInteract_Implementation(AActor* Interactor) override;
	virtual FText GetInteractionPrompt_Implementation() const override { return FText::FromString(TEXT("Pilot")); }

	AVehicleGridActor* GetVehicle() const;
};
