// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PowerNetworkComponent.generated.h"

class UPowerComponent;

USTRUCT(BlueprintType)
struct FPowerNetworkSnapshot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) float TotalProduction = 0.f;
	UPROPERTY(BlueprintReadOnly) float TotalDemand = 0.f;
	UPROPERTY(BlueprintReadOnly) float TotalStored = 0.f;
	UPROPERTY(BlueprintReadOnly) float TotalStorage = 0.f;
	UPROPERTY(BlueprintReadOnly) bool bOverload = false;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPowerNetworkUpdated, const FPowerNetworkSnapshot&, Snapshot);

/**
 * Owns the power simulation for a single grid (base or vehicle). Sums all
 * UPowerComponents on blocks owned by the grid and distributes available
 * energy proportionally to consumers each tick.
 */
UCLASS(ClassGroup = (Exoneer), meta = (BlueprintSpawnableComponent))
class EXONEER_API UPowerNetworkComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPowerNetworkComponent();

	UPROPERTY(BlueprintAssignable) FOnPowerNetworkUpdated OnPowerNetworkUpdated;

	UFUNCTION(BlueprintCallable, Category = "Power") void Register(UPowerComponent* Node);
	UFUNCTION(BlueprintCallable, Category = "Power") void Unregister(UPowerComponent* Node);

	UFUNCTION(BlueprintPure, Category = "Power") const FPowerNetworkSnapshot& GetSnapshot() const { return LastSnapshot; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn) override;

protected:
	UPROPERTY() TArray<UPowerComponent*> Nodes;
	FPowerNetworkSnapshot LastSnapshot;
};
