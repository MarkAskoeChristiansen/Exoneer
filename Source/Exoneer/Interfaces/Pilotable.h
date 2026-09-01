// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "Vehicles/PilotInput.h"
#include "Pilotable.generated.h"

UINTERFACE(BlueprintType, MinimalAPI)
class UPilotable : public UInterface { GENERATED_BODY() };

class APawn;

/**
 * Implemented by AVehicleConstruct. StationId identifies WHICH cockpit on the
 * construct is being used (the cockpit's BlockInstanceId); constructs with a
 * single seat may ignore it. All functions are SERVER-ONLY; pilot input
 * arrives via the pilot pawn's own Server RPC and is forwarded here.
 */
class EXONEER_API IPilotable
{
	GENERATED_BODY()
public:
	/** SERVER. Seat the pawn at the given station. Returns false if occupied/invalid. */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Vehicle")
	bool EnterPilot(APawn* Pilot, int32 StationId);
	virtual bool EnterPilot_Implementation(APawn* Pilot, int32 StationId) { return false; }

	/** SERVER. Release the pawn from its station. */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Vehicle")
	void ExitPilot(APawn* Pilot);
	virtual void ExitPilot_Implementation(APawn* Pilot) {}

	/** SERVER. Pilot intent packet (held until the next one or a timeout); consumed by the physics tick. */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Vehicle")
	void ApplyPilotInput(const FPilotInput& Input);
	virtual void ApplyPilotInput_Implementation(const FPilotInput& Input) {}
};
