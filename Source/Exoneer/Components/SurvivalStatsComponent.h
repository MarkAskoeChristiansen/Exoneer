// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ExoneerTypes.h"
#include "SurvivalStatsComponent.generated.h"

class UInventoryComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStatChanged, float, NormalizedValue);

/**
 * Holds the player's survival vitals: suit O2 (litres), suit power (kJ),
 * body temperature, and the suit seal's causal condition (leak L/s, patch
 * count). No hunger/thirst by design (GAME-SCOPE.md module 1). Health lives
 * in UHealthComponent.
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

	// ---- Oxygen (litres) ----
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|Oxygen") float SuitO2CapacityL = 100.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|Oxygen") float MetabolicO2Lps = 0.05f;
	UPROPERTY(ReplicatedUsing = OnRep_Oxygen, VisibleInstanceOnly, BlueprintReadOnly, Category = "Survival|Oxygen") float Oxygen = 100.f;
	UPROPERTY(BlueprintAssignable) FOnStatChanged OnOxygenChanged;

	// ---- Suit Power (kJ, drain in watts) ----
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|SuitPower") float SuitPowerCapacityKJ = 1800.f;
	/** Idle draw (W). 540 W empties 1800 kJ in the same 3333 s the old 100/0.03 units did. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|SuitPower") float SuitPowerDrainW = 540.f;
	UPROPERTY(ReplicatedUsing = OnRep_SuitPower, VisibleInstanceOnly, BlueprintReadOnly, Category = "Survival|SuitPower") float SuitPower = 1800.f;
	UPROPERTY(BlueprintAssignable) FOnStatChanged OnSuitPowerChanged;

	// ---- Suit seal (causal condition) ----
	UPROPERTY(Replicated, VisibleInstanceOnly, BlueprintReadOnly, Category = "Survival|Seal")
	FPartCondition SuitCondition;

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

	// ---- Public mutation (SERVER) ----
	UFUNCTION(BlueprintCallable) void AddOxygen(float Amount);
	UFUNCTION(BlueprintCallable) void AddSuitPower(float Amount);
	UFUNCTION(BlueprintCallable) void SetAmbientTemperature(float TempCelsius) { AmbientTempC = TempCelsius; }

	/** SERVER. Grow the seal leak (landing impact, storm exposure). */
	void AddLeakRateLps(float DeltaLps);

	/**
	 * SERVER. Consume one seal_kit and patch: leak becomes 0.005*(1+PatchCount)
	 * then PatchCount increments. Refused at the cap, with no leak, or without
	 * a kit in Source.
	 */
	bool TryPatchSeal(UInventoryComponent* Source);

	/**
	 * SERVER. Consume one suit_seal and reset leak and PatchCount to 0.
	 * Legal only at the patch cap.
	 */
	bool TryReplaceSeal(UInventoryComponent* Source);

	UFUNCTION(BlueprintPure) float GetOxygenNormalized() const { return SuitO2CapacityL > 0.f ? Oxygen / SuitO2CapacityL : 0.f; }
	UFUNCTION(BlueprintPure) float GetSuitPowerNormalized() const { return SuitPowerCapacityKJ > 0.f ? SuitPower / SuitPowerCapacityKJ : 0.f; }
	UFUNCTION(BlueprintPure) float GetBodyTemperature() const { return BodyTempC; }

	/** Metabolic plus leak, L/s. */
	float GetOxygenDrainLps() const;

	/** Idle drain in kJ/s (watts / 1000). */
	float GetSuitPowerDrainKJps() const { return FMath::Max(SuitPowerDrainW, 0.f) / 1000.f; }

protected:
	UFUNCTION() void OnRep_Oxygen()    { OnOxygenChanged.Broadcast(GetOxygenNormalized()); }
	UFUNCTION() void OnRep_SuitPower() { OnSuitPowerChanged.Broadcast(GetSuitPowerNormalized()); }
	UFUNCTION() void OnRep_BodyTemp()  { OnTemperatureChanged.Broadcast(BodyTempC); }
};
