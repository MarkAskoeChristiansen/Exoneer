// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "PowerProducer.generated.h"

UINTERFACE(BlueprintType, MinimalAPI)
class UPowerProducer : public UInterface { GENERATED_BODY() };

class EXONEER_API IPowerProducer
{
	GENERATED_BODY()
public:
	/** Watts available this frame (e.g. solar yield, generator output). */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Power")
	float GetPowerOutput() const;
	virtual float GetPowerOutput_Implementation() const { return 0.f; }

	/** True for batteries: can both supply and store. */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Power")
	bool IsStorage() const;
	virtual bool IsStorage_Implementation() const { return false; }
};
