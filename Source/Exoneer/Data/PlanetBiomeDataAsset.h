// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "PlanetBiomeDataAsset.generated.h"

class UItemDefinitionDataAsset;

USTRUCT(BlueprintType)
struct FBiomeResourceSpawn
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Biome")
	TSoftObjectPtr<UItemDefinitionDataAsset> Resource;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Biome", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SpawnWeight = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Biome")
	int32 MinPerNode = 5;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Biome")
	int32 MaxPerNode = 25;
};

/**
 * Describes a planetary biome / environment profile. The starter planet uses
 * one of these by default but multiple biomes can coexist on one world.
 */
UCLASS(BlueprintType)
class EXONEER_API UPlanetBiomeDataAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AssetRegistrySearchable, Category = "Biome")
	FName BiomeId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Biome")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Biome")
	float GravityZ = -980.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Biome")
	float AtmosphereOxygen = 0.f;          // 0..1 ambient oxygen fraction

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Biome")
	float DayTempCelsius = 25.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Biome")
	float NightTempCelsius = -40.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Biome")
	float WindSpeed = 2.f;                  // m/s — affects wind turbine output

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Biome")
	float StormProbabilityPerHour = 0.05f;

	/** Structure damage per second from a full-intensity storm, before piece StormResistance. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Biome", meta = (ClampMin = "0"))
	float StormDamagePerSecond = 2.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Biome")
	float RadiationLevel = 0.f;             // 0..1

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Biome")
	TArray<FBiomeResourceSpawn> ResourceSpawns;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Biome")
	FLinearColor SkyHorizonColor = FLinearColor(0.6f, 0.4f, 0.8f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Biome")
	FLinearColor SkyZenithColor = FLinearColor(0.1f, 0.05f, 0.3f);

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId(TEXT("Biome"), BiomeId);
	}
};
