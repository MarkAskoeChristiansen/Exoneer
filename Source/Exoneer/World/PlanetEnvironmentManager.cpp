// Copyright Exoneer contributors.
#include "World/PlanetEnvironmentManager.h"
#include "Data/PlanetBiomeDataAsset.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"

APlanetEnvironmentManager::APlanetEnvironmentManager()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.1f;
}

float APlanetEnvironmentManager::GetSunExposureFraction() const
{
	// Simple bell: peaks at TimeOfDay=0.5 (noon), zero at midnight.
	const float X = (TimeOfDay - 0.5f) * 2.f * PI;
	const float V = FMath::Cos(X);
	float Frac = FMath::Clamp(V, 0.f, 1.f);
	if (bStormActive) Frac *= 0.25f;
	return Frac;
}

bool APlanetEnvironmentManager::IsNight() const
{
	return TimeOfDay < 0.2f || TimeOfDay > 0.8f;
}

float APlanetEnvironmentManager::GetCurrentAmbientTemperatureC() const
{
	if (!Biome) return 20.f;
	return FMath::Lerp(Biome->NightTempCelsius, Biome->DayTempCelsius, GetSunExposureFraction());
}

void APlanetEnvironmentManager::UpdateSun()
{
	if (!SunLight) return;
	UDirectionalLightComponent* L = SunLight->GetComponent();
	if (!L) return;
	const float Pitch = FMath::Lerp(-90.f, 90.f, TimeOfDay);
	L->SetWorldRotation(FRotator(Pitch, 30.f, 0.f));
}

void APlanetEnvironmentManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (SecondsPerDay > 0.f)
	{
		TimeOfDay = FMath::Fmod(TimeOfDay + DeltaSeconds / SecondsPerDay, 1.f);
	}

	UpdateSun();

	// Random storm rolling.
	StormTimer += DeltaSeconds;
	if (StormTimer >= 60.f && Biome)
	{
		StormTimer = 0.f;
		const float Roll = FMath::FRand();
		bStormActive = Roll < Biome->StormProbabilityPerHour;
	}

	OnTimeOfDayChanged.Broadcast(TimeOfDay);
}
