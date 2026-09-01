// Copyright Exoneer contributors.
#include "World/PlanetEnvironmentManager.h"
#include "Data/PlanetBiomeDataAsset.h"
#include "Data/PieceDefinitionDataAsset.h"
#include "Building/BaseStructure.h"
#include "Building/BasePiece.h"
#include "Components/ConstructionComponent.h"
#include "Interfaces/Damageable.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"

APlanetEnvironmentManager::APlanetEnvironmentManager()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.1f;
	bReplicates = true;
	// One per world, everyone needs its clock and storm state.
	bAlwaysRelevant = true;
	// Clients advance time locally; low-frequency corrections are enough.
	SetNetUpdateFrequency(2.f);
}

void APlanetEnvironmentManager::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APlanetEnvironmentManager, TimeOfDay01);
	DOREPLIFETIME(APlanetEnvironmentManager, bStormActive);
	DOREPLIFETIME(APlanetEnvironmentManager, StormIntensity);
}

float APlanetEnvironmentManager::GetSunFraction() const
{
	// Simple bell: peaks at TimeOfDay01 = 0.5 (noon), zero at midnight.
	const float X = (TimeOfDay01 - 0.5f) * 2.f * PI;
	const float V = FMath::Cos(X);
	float Frac = FMath::Clamp(V, 0.f, 1.f);
	if (bStormActive)
	{
		// Storm clouds cut irradiance down to 25% at full intensity.
		Frac *= FMath::Lerp(1.f, 0.25f, FMath::Clamp(StormIntensity, 0.f, 1.f));
	}
	return Frac;
}

bool APlanetEnvironmentManager::IsNight() const
{
	return TimeOfDay01 < 0.2f || TimeOfDay01 > 0.8f;
}

float APlanetEnvironmentManager::GetCurrentAmbientTemperatureC() const
{
	if (!Biome) return 20.f;
	return FMath::Lerp(Biome->NightTempCelsius, Biome->DayTempCelsius, GetSunFraction());
}

void APlanetEnvironmentManager::UpdateSun()
{
	if (!SunLight) return;
	UDirectionalLightComponent* L = SunLight->GetComponent();
	if (!L) return;

	// The day/night cycle rotates the light every tick; a Static/Stationary
	// sun spams mobility warnings each frame and never moves. Promote it once
	// so the manager works regardless of how the map author set the light up.
	if (L->Mobility != EComponentMobility::Movable)
	{
		L->SetMobility(EComponentMobility::Movable);
	}

	const float Pitch = FMath::Lerp(-90.f, 90.f, TimeOfDay01);
	L->SetWorldRotation(FRotator(Pitch, 30.f, 0.f));
}

void APlanetEnvironmentManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Every machine advances the clock locally; the server's replicated
	// value periodically corrects client drift.
	if (SecondsPerDay > 0.f)
	{
		TimeOfDay01 = FMath::Fmod(TimeOfDay01 + DeltaSeconds / SecondsPerDay, 1.f);
	}

	// All machines rotate the sun from the shared clock so skies match.
	UpdateSun();

	if (HasAuthority())
	{
		UpdateStormSchedule(DeltaSeconds);

		// Storm structure damage, cadence 1/s while active.
		if (bStormActive)
		{
			StormDamageTimer += DeltaSeconds;
			while (StormDamageTimer >= 1.f)
			{
				StormDamageTimer -= 1.f;
				ApplyStormDamage();
			}
		}
		else
		{
			StormDamageTimer = 0.f;
		}
	}

	OnTimeOfDayChanged.Broadcast(TimeOfDay01);
}

void APlanetEnvironmentManager::UpdateStormSchedule(float DeltaSeconds)
{
	// An active storm lives for its rolled duration, then ends.
	if (bStormActive)
	{
		StormSecondsRemaining -= DeltaSeconds;
		if (StormSecondsRemaining <= 0.f)
		{
			bStormActive = false;
			StormIntensity = 0.f;
			ExposureCache.Empty();
			CachedStructurePieceCounts.Empty();
			OnRep_Storm(); // The listen host takes the client notification path too.
		}
		return;
	}

	// Start rolls happen once per minute at the per-MINUTE probability, so the
	// authored per-hour field means what it says (rolling the hourly value
	// every minute made storms ~60x too frequent and one minute long).
	StormTimer += DeltaSeconds;
	if (StormTimer >= 60.f && Biome)
	{
		StormTimer = 0.f;
		if (FMath::FRand() < Biome->StormProbabilityPerHour / 60.f)
		{
			bStormActive = true;
			StormIntensity = FMath::FRandRange(0.5f, 1.f);
			StormSecondsRemaining = FMath::FRandRange(
				FMath::Min(StormMinDurationSeconds, StormMaxDurationSeconds),
				FMath::Max(StormMinDurationSeconds, StormMaxDurationSeconds));

			// Fresh cover geometry for this storm.
			ExposureCache.Empty();
			CachedStructurePieceCounts.Empty();
			OnRep_Storm();
		}
	}
}

void APlanetEnvironmentManager::OnRep_Storm()
{
	OnStormChangedBP(bStormActive, StormIntensity);
}

void APlanetEnvironmentManager::ApplyStormDamage()
{
	UWorld* World = GetWorld();
	if (!World || !Biome) return;

	const float BaseDamage = Biome->StormDamagePerSecond * FMath::Clamp(StormIntensity, 0.f, 1.f);
	if (BaseDamage <= 0.f) return;

	for (TActorIterator<ABaseStructure> It(World); It; ++It)
	{
		ABaseStructure* Structure = *It;
		if (!IsValid(Structure)) continue;

		// Any piece-count change reshapes cover: drop this structure's
		// cached exposure results before reading them.
		const int32 Count = Structure->GetPieceCount();
		int32& CachedCount = CachedStructurePieceCounts.FindOrAdd(Structure, INDEX_NONE);
		if (CachedCount != Count)
		{
			CachedCount = Count;
			for (auto CacheIt = ExposureCache.CreateIterator(); CacheIt; ++CacheIt)
			{
				const ABasePiece* P = CacheIt.Key().Get();
				if (!P || P->OwningStructure == Structure)
				{
					CacheIt.RemoveCurrent();
				}
			}
		}

		for (ABasePiece* Piece : Structure->Pieces)
		{
			// Ghosts and construction sites are spared; only COMPLETE pieces weather.
			if (!IsValid(Piece) || !Piece->Construction || !Piece->Construction->IsComplete()) continue;
			if (!IsPieceExposed(Piece)) continue;

			const float Resist = Piece->Def ? FMath::Clamp(Piece->Def->StormResistance, 0.f, 1.f) : 0.f;
			const float Damage = BaseDamage * (1.f - Resist);
			if (Damage > 0.f)
			{
				IDamageable::Execute_ApplyExoneerDamage(Piece, Damage, EExoneerDamageType::Impact, this);
			}
		}
	}
}

bool APlanetEnvironmentManager::IsPieceExposed(ABasePiece* Piece)
{
	if (const bool* Cached = ExposureCache.Find(Piece))
	{
		return *Cached;
	}

	// Upward trace from just above the piece bounds; a blocking hit means
	// something (a roof, another piece, terrain) covers this piece.
	const FBox Bounds = Piece->GetComponentsBoundingBox();
	const FVector Center = Bounds.GetCenter();
	const FVector Start(Center.X, Center.Y, Bounds.Max.Z + 10.f);
	const FVector End = Start + FVector(0.f, 0.f, 50000.f);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(StormExposure), /*bTraceComplex*/ false, Piece);
	FHitResult Hit;
	const bool bCovered = GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);

	ExposureCache.Add(Piece, !bCovered);
	return !bCovered;
}
