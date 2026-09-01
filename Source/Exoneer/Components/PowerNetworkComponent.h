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

	UPROPERTY(BlueprintReadOnly, Category = "Power") float TotalProduction = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Power") float TotalDemand = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Power") float TotalStored = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Power") float TotalStorage = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Power") bool bOverload = false;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPowerNetworkUpdated, const FPowerNetworkSnapshot&, Snapshot);

/**
 * Owns the power simulation for one base structure. Sums all registered
 * UPowerComponents (COMPLETE pieces only), covers deficits from batteries,
 * charges them with surplus, and writes a SupplyFraction back to consumers.
 *
 * Simulates on the SERVER at SimInterval; the snapshot replicates for HUD.
 * (Vehicle constructs run their own simplified ledger over block records.)
 */
UCLASS(ClassGroup = (Exoneer), meta = (BlueprintSpawnableComponent))
class EXONEER_API UPowerNetworkComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPowerNetworkComponent();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Power") float SimInterval = 0.2f;

	UPROPERTY(BlueprintAssignable) FOnPowerNetworkUpdated OnPowerNetworkUpdated;

	UFUNCTION(BlueprintCallable, Category = "Power") void Register(UPowerComponent* Node);
	UFUNCTION(BlueprintCallable, Category = "Power") void Unregister(UPowerComponent* Node);

	UFUNCTION(BlueprintPure, Category = "Power") const FPowerNetworkSnapshot& GetSnapshot() const { return Snapshot; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	UPROPERTY() TArray<TObjectPtr<UPowerComponent>> Nodes;

	UPROPERTY(ReplicatedUsing = OnRep_Snapshot)
	FPowerNetworkSnapshot Snapshot;

	UFUNCTION() void OnRep_Snapshot();

	void Simulate(float DeltaSeconds);
};
