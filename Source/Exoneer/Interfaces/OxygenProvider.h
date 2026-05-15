// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "OxygenProvider.generated.h"

UINTERFACE(BlueprintType, MinimalAPI)
class UOxygenProvider : public UInterface { GENERATED_BODY() };

class EXONEER_API IOxygenProvider
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Oxygen")
	float GetStoredOxygen() const;
	virtual float GetStoredOxygen_Implementation() const { return 0.f; }

	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Oxygen")
	float WithdrawOxygen(float Amount);
	virtual float WithdrawOxygen_Implementation(float Amount) { return 0.f; }
};
