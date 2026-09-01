// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PlanetEnvironmentManager.generated.h"

class UPlanetBiomeDataAsset;
class UExoneerSoilPhysicalMaterial;
class ADirectionalLight;
class ABasePiece;
class ABaseStructure;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnTimeOfDayChanged, float, NormalizedTimeOfDay);

/**
 * Drives the day/night cycle, ambient temperature, and storms, and exposes a
 * "sun fraction" used by solar panels. Designed to be replaced by a real
 * spherical-planet system later.
 *
 * Network split: the SERVER owns time and storm state. TimeOfDay01 replicates
 * as a plain float and every machine keeps advancing it locally between
 * updates; the sun light rotates from it everywhere so all players share one
 * sky. Storm state replicates with a RepNotify that feeds the BP FX hook.
 * While a storm is active the server damages exposed COMPLETE base pieces
 * once per second, mitigated by each piece definition's StormResistance.
 */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API APlanetEnvironmentManager : public AActor
{
	GENERATED_BODY()
public:
	APlanetEnvironmentManager();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet") TObjectPtr<UPlanetBiomeDataAsset> Biome;

	/** Real seconds per in-game day. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet") float SecondsPerDay = 600.f;

	/** Optional: directional light used as the sun (set in Editor). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet") TObjectPtr<ADirectionalLight> SunLight;

	/** Normalized time of day, 0..1 (0.5 = noon). Was 'TimeOfDay' in v1. */
	UPROPERTY(Replicated, VisibleInstanceOnly, BlueprintReadOnly, Category = "Planet") float TimeOfDay01 = 0.25f;

	UPROPERTY(BlueprintAssignable) FOnTimeOfDayChanged OnTimeOfDayChanged;

	/** 0..1 solar irradiance right now (day/night bell, storm attenuation). */
	UFUNCTION(BlueprintPure, Category = "Planet") float GetSunFraction() const;

	/** Legacy alias for GetSunFraction, kept for v1 callers. */
	UFUNCTION(BlueprintPure, Category = "Planet") float GetSunExposureFraction() const { return GetSunFraction(); }

	/** Biome default substrate for wheels; null when the biome declares none (firm ground). */
	const UExoneerSoilPhysicalMaterial* GetDefaultSoil() const;

	UFUNCTION(BlueprintPure, Category = "Planet") float GetCurrentAmbientTemperatureC() const;
	UFUNCTION(BlueprintPure, Category = "Planet") bool IsNight() const;
	UFUNCTION(BlueprintPure, Category = "Planet") bool IsStormActive() const { return bStormActive; }
	UFUNCTION(BlueprintPure, Category = "Planet") float GetStormIntensity() const { return StormIntensity; }

	/** BP hook for wind, audio, and post FX; fires on every machine when storm state changes. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Planet")
	void OnStormChangedBP(bool bActive, float Intensity);

	virtual void Tick(float DeltaSeconds) override;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	/** Wires Biome->GravityZ into world settings on both sides - one gravity source for physics, characters, and wheels. */
	virtual void BeginPlay() override;

	UPROPERTY(ReplicatedUsing = OnRep_Storm)
	bool bStormActive = false;

	/** 0..1 severity of the active storm; scales structure damage and solar loss. */
	UPROPERTY(ReplicatedUsing = OnRep_Storm)
	float StormIntensity = 0.f;

	/** Storm length range; the exact duration rolls when a storm starts. */
	UPROPERTY(EditAnywhere, Category = "Planet") float StormMinDurationSeconds = 90.f;
	UPROPERTY(EditAnywhere, Category = "Planet") float StormMaxDurationSeconds = 240.f;

	/** SERVER. Seconds since the last storm scheduling roll. */
	float StormTimer = 0.f;

	/** SERVER. Remaining lifetime of the active storm. */
	float StormSecondsRemaining = 0.f;

	/** SERVER. Seconds of storm structure damage still owed. */
	float StormDamageTimer = 0.f;

	/** SERVER. Cached exposure per piece; invalidated when its structure's piece count changes. */
	TMap<TWeakObjectPtr<ABasePiece>, bool> ExposureCache;

	/** SERVER. Piece count each structure had when its pieces were cached. */
	TMap<TWeakObjectPtr<ABaseStructure>, int32> CachedStructurePieceCounts;

	UFUNCTION() void OnRep_Storm();

	/** Rotate the sun light from TimeOfDay01. Runs on every machine. */
	void UpdateSun();

	/** SERVER. Roll the storm schedule (once per minute, v1 cadence). */
	void UpdateStormSchedule(float DeltaSeconds);

	/** SERVER. One second of storm damage to every exposed COMPLETE piece. */
	void ApplyStormDamage();

	/** SERVER. Exposure = no blocking hit on an upward trace from the piece bounds top + 10 cm. */
	bool IsPieceExposed(ABasePiece* Piece);
};
