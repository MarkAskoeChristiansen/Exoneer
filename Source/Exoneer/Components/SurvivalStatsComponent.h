// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SurvivalStatsComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStatChanged, float, NormalizedValue);

/**
 * Holds the player's survival "vitals" — Oxygen, SuitPower, Temperature.
 * No hunger/thirst by design (GAME-SCOPE.md module 1: tension comes from
 * engineering failures and suit resources, not biological micromanagement).
 * Health lives in UHealthComponent.
 *
 * Values are stored in raw units; UI consumes the normalized 0..1 helpers.
 * Survival damage is applied via the owning actor's IDamageable interface.
 *
 * SERVER simulates the drains and applies critical damage; the raw values
 * replicate with RepNotify broadcasts so client HUDs track the authoritative
 * state (tools drain suit power server-side - without replication the owning
 * client's local copy would silently diverge).
 */
UCLASS(ClassGroup = (Exoneer), meta = (BlueprintSpawnableComponent))
class EXONEER_API USurvivalStatsComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USurvivalStatsComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// ---- Oxygen ----
	// Prototype-friendly default drains: no refill loops are wired yet, so the
	// suit lasts a play session. Tighten these when umbilical recharge and
	// oxygen consumption land (GAME-SCOPE.md module 1).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|Oxygen") float MaxOxygen = 100.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|Oxygen") float OxygenDrainPerSec = 0.05f;
	UPROPERTY(ReplicatedUsing = OnRep_Oxygen, VisibleInstanceOnly, BlueprintReadOnly, Category = "Survival|Oxygen") float Oxygen = 100.f;
	UPROPERTY(BlueprintAssignable) FOnStatChanged OnOxygenChanged;

	// ---- Suit Power ----
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|SuitPower") float MaxSuitPower = 100.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|SuitPower") float SuitPowerDrainPerSec = 0.03f;
	UPROPERTY(ReplicatedUsing = OnRep_SuitPower, VisibleInstanceOnly, BlueprintReadOnly, Category = "Survival|SuitPower") float SuitPower = 100.f;
	UPROPERTY(BlueprintAssignable) FOnStatChanged OnSuitPowerChanged;

	// ---- Temperature (body) ----
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|Temperature") float MinSafeTempC = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|Temperature") float MaxSafeTempC = 40.f;
	UPROPERTY(ReplicatedUsing = OnRep_BodyTemp, VisibleInstanceOnly, BlueprintReadOnly, Category = "Survival|Temperature") float BodyTempC = 36.6f;
	UPROPERTY(Replicated, VisibleInstanceOnly, BlueprintReadOnly, Category = "Survival|Temperature") float AmbientTempC = 20.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|Temperature") float TempEquilibrationRate = 0.05f;
	UPROPERTY(BlueprintAssignable) FOnStatChanged OnTemperatureChanged;

	// ---- Damage rates when stats are critical ----
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|Damage") float SuffocationDPS = 8.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|Damage") float TempDamageDPS = 4.f;

	// ---- Public mutation ----
	UFUNCTION(BlueprintCallable) void AddOxygen(float Amount);
	UFUNCTION(BlueprintCallable) void AddSuitPower(float Amount);
	UFUNCTION(BlueprintCallable) void SetAmbientTemperature(float TempCelsius) { AmbientTempC = TempCelsius; }

	UFUNCTION(BlueprintPure) float GetOxygenNormalized() const { return MaxOxygen > 0 ? Oxygen / MaxOxygen : 0.f; }
	UFUNCTION(BlueprintPure) float GetSuitPowerNormalized() const { return MaxSuitPower > 0 ? SuitPower / MaxSuitPower : 0.f; }
	UFUNCTION(BlueprintPure) float GetBodyTemperature() const { return BodyTempC; }

protected:
	UFUNCTION() void OnRep_Oxygen()    { OnOxygenChanged.Broadcast(GetOxygenNormalized()); }
	UFUNCTION() void OnRep_SuitPower() { OnSuitPowerChanged.Broadcast(GetSuitPowerNormalized()); }
	UFUNCTION() void OnRep_BodyTemp()  { OnTemperatureChanged.Broadcast(BodyTempC); }
};
