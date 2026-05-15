// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PlanetEnvironmentManager.generated.h"

class UPlanetBiomeDataAsset;
class ADirectionalLight;
class APostProcessVolume;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnTimeOfDayChanged, float, NormalizedTimeOfDay);

/**
 * Drives day/night cycle, ambient temperature, storms, and exposes a
 * "sun fraction" used by solar panels. Designed to be replaced by a real
 * spherical-planet system later.
 */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API APlanetEnvironmentManager : public AActor
{
	GENERATED_BODY()
public:
	APlanetEnvironmentManager();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet") UPlanetBiomeDataAsset* Biome = nullptr;

	/** Real seconds per in-game day. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet") float SecondsPerDay = 600.f;

	/** Optional: directional light used as the sun (set in Editor). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet") ADirectionalLight* SunLight = nullptr;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly) float TimeOfDay = 0.25f; // 0..1

	UPROPERTY(BlueprintAssignable) FOnTimeOfDayChanged OnTimeOfDayChanged;

	UFUNCTION(BlueprintPure, Category = "Planet") float GetSunExposureFraction() const;
	UFUNCTION(BlueprintPure, Category = "Planet") float GetCurrentAmbientTemperatureC() const;
	UFUNCTION(BlueprintPure, Category = "Planet") bool IsNight() const;
	UFUNCTION(BlueprintPure, Category = "Planet") bool IsStormActive() const { return bStormActive; }

	virtual void Tick(float DeltaSeconds) override;

protected:
	bool bStormActive = false;
	float StormTimer = 0.f;

	void UpdateSun();
};
