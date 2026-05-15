// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SurvivalStatsComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnStatChanged, float, NormalizedValue);

/**
 * Holds the player's survival "vitals" — Oxygen, SuitPower, Nutrition,
 * Temperature. Health lives in UHealthComponent.
 *
 * Values are stored in raw units; UI consumes the normalized 0..1 helpers.
 * Survival damage is applied via the owning actor's IDamageable interface.
 */
UCLASS(ClassGroup = (Exoneer), meta = (BlueprintSpawnableComponent))
class EXONEER_API USurvivalStatsComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USurvivalStatsComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn) override;

	// ---- Oxygen ----
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|Oxygen") float MaxOxygen = 100.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|Oxygen") float OxygenDrainPerSec = 0.5f;
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Survival|Oxygen") float Oxygen = 100.f;
	UPROPERTY(BlueprintAssignable) FOnStatChanged OnOxygenChanged;

	// ---- Suit Power ----
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|SuitPower") float MaxSuitPower = 100.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|SuitPower") float SuitPowerDrainPerSec = 0.2f;
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Survival|SuitPower") float SuitPower = 100.f;
	UPROPERTY(BlueprintAssignable) FOnStatChanged OnSuitPowerChanged;

	// ---- Nutrition ----
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|Nutrition") float MaxNutrition = 100.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|Nutrition") float NutritionDrainPerSec = 0.05f;
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Survival|Nutrition") float Nutrition = 100.f;
	UPROPERTY(BlueprintAssignable) FOnStatChanged OnNutritionChanged;

	// ---- Temperature (body) ----
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|Temperature") float MinSafeTempC = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|Temperature") float MaxSafeTempC = 40.f;
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Survival|Temperature") float BodyTempC = 36.6f;
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Survival|Temperature") float AmbientTempC = 20.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|Temperature") float TempEquilibrationRate = 0.05f;
	UPROPERTY(BlueprintAssignable) FOnStatChanged OnTemperatureChanged;

	// ---- Damage rates when stats are critical ----
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|Damage") float SuffocationDPS = 8.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|Damage") float StarvationDPS = 1.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Survival|Damage") float TempDamageDPS = 4.f;

	// ---- Public mutation ----
	UFUNCTION(BlueprintCallable) void AddOxygen(float Amount);
	UFUNCTION(BlueprintCallable) void AddSuitPower(float Amount);
	UFUNCTION(BlueprintCallable) void AddNutrition(float Amount);
	UFUNCTION(BlueprintCallable) void SetAmbientTemperature(float TempCelsius) { AmbientTempC = TempCelsius; }

	UFUNCTION(BlueprintPure) float GetOxygenNormalized() const { return MaxOxygen > 0 ? Oxygen / MaxOxygen : 0.f; }
	UFUNCTION(BlueprintPure) float GetSuitPowerNormalized() const { return MaxSuitPower > 0 ? SuitPower / MaxSuitPower : 0.f; }
	UFUNCTION(BlueprintPure) float GetNutritionNormalized() const { return MaxNutrition > 0 ? Nutrition / MaxNutrition : 0.f; }
	UFUNCTION(BlueprintPure) float GetBodyTemperature() const { return BodyTempC; }
};
