// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "PowerConsumer.generated.h"

UINTERFACE(BlueprintType, MinimalAPI)
class UPowerConsumer : public UInterface { GENERATED_BODY() };

class EXONEER_API IPowerConsumer
{
	GENERATED_BODY()
public:
	/** Watts the actor wants to draw if fully powered. */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Power")
	float GetPowerDraw() const;
	virtual float GetPowerDraw_Implementation() const { return 0.f; }

	/** Called by network with how much it actually received (<= draw). */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Power")
	void OnPowerSupplied(float Watts);
	virtual void OnPowerSupplied_Implementation(float Watts) {}
};
