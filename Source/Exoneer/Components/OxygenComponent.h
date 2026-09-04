// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Interfaces/OxygenProvider.h"
#include "OxygenComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnOxygenStorageChanged, float, Stored, float, Capacity);

/**
 * Holds an oxygen reservoir for tanks / generators / sealed rooms.
 */
UCLASS(ClassGroup = (Exoneer), meta = (BlueprintSpawnableComponent))
class EXONEER_API UOxygenComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UOxygenComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Oxygen") float Capacity = 100.f;
	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Oxygen") float Stored = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Oxygen") float ProductionPerSec = 0.f;

	UPROPERTY(BlueprintAssignable) FOnOxygenStorageChanged OnOxygenChanged;

	UFUNCTION(BlueprintCallable) float Deposit(float Amount);
	UFUNCTION(BlueprintCallable) float Withdraw(float Amount);

	UFUNCTION(BlueprintPure) float GetFillFraction() const { return Capacity > 0 ? Stored / Capacity : 0.f; }

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
};
