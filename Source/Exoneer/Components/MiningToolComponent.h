// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MiningToolComponent.generated.h"

class UInventoryComponent;
class AResourceNode;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnMiningProgress, float, Progress);

/**
 * Held by the player. When PrimaryAction is held, sweeps forward, finds a
 * AResourceNode and progressively extracts items into the player's inventory.
 */
UCLASS(ClassGroup = (Exoneer), meta = (BlueprintSpawnableComponent))
class EXONEER_API UMiningToolComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UMiningToolComponent();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mining") float Range = 250.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mining") float Radius = 8.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mining") float DamagePerSec = 25.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mining") float SuitPowerDrainPerSec = 2.f;

	UPROPERTY(BlueprintAssignable) FOnMiningProgress OnMiningProgress;

	UFUNCTION(BlueprintCallable, Category = "Mining") void SetActive(bool bNewActive) { bActive = bNewActive; }
	UFUNCTION(BlueprintPure, Category = "Mining") bool IsActive() const { return bActive; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn) override;

protected:
	bool bActive = false;
};
