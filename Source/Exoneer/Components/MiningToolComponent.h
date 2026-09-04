// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/NetSerialization.h"
#include "MiningToolComponent.generated.h"

class UInventoryComponent;
class AResourceNode;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnMiningProgress, float, Progress);

/**
 * Held by the player. While active, the locally controlled pawn traces
 * forward each tick for beam FX and sends batched Server_MineTarget intents
 * (~MineRateHz). The SERVER validates range and rate, applies damage to the
 * node, and deposits yield into the owning player's inventory.
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
	/** Suit power drain (kJ/s). 9 kJ/s keeps mining's share of the 1800 kJ bank identical to the old 0.5/100 units. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mining") float SuitPowerDrainPerSec = 9.f;

	/** How often mining intents are sent to the server while the beam is on. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mining") float MineRateHz = 5.f;

	/** Server range check slack factor (latency). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mining") float ServerRangeSlack = 1.5f;

	UPROPERTY(BlueprintAssignable) FOnMiningProgress OnMiningProgress;

	// Named to avoid shadowing UActorComponent::SetActive/IsActive (UHT rejects that).
	UFUNCTION(BlueprintCallable, Category = "Mining") void SetMiningActive(bool bNewActive) { bActive = bNewActive; }
	UFUNCTION(BlueprintPure, Category = "Mining") bool IsMiningActive() const { return bActive; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn) override;

protected:
	bool bActive = false;
	float IntentAccumulator = 0.f;

	/** SERVER. Rate limiter: seconds of mining damage still owed to this client. */
	float ServerDamageBudget = 0.f;
	double ServerLastMineTime = 0.0;

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_MineTarget(AResourceNode* Node, FVector_NetQuantize HitPoint, float Seconds);
};
