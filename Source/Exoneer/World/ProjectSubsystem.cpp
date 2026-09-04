// Copyright Exoneer contributors.
#include "World/ProjectSubsystem.h"
#include "World/ExoneerGameState.h"
#include "World/PlanetEnvironmentManager.h"
#include "Data/ProjectDefinitionDataAsset.h"
#include "Building/BaseStructure.h"
#include "Building/BasePiece.h"
#include "Data/PieceDefinitionDataAsset.h"
#include "Components/ConstructionComponent.h"
#include "Components/PowerNetworkComponent.h"
#include "Components/OxygenComponent.h"
#include "Components/SurvivalStatsComponent.h"
#include "Player/PlayerSurvivalCharacter.h"
#include "Vehicles/VehicleConstruct.h"
#include "Engine/AssetManager.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Exoneer.h"

void UProjectSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

AExoneerGameState* UProjectSubsystem::GetGS() const
{
	UWorld* World = GetWorld();
	return World ? World->GetGameState<AExoneerGameState>() : nullptr;
}

UProjectDefinitionDataAsset* UProjectSubsystem::FindDef(FName ProjectId) const
{
	FPrimaryAssetId Id(TEXT("Project"), ProjectId);
	return Cast<UProjectDefinitionDataAsset>(UAssetManager::Get().GetPrimaryAssetObject(Id));
}

void UProjectSubsystem::EnsureCatalog()
{
	AExoneerGameState* GS = GetGS();
	if (!GS || !GS->HasAuthority())
	{
		return;
	}
	if (GS->Projects.Num() > 0)
	{
		return;
	}

	TArray<FPrimaryAssetId> Ids;
	UAssetManager::Get().GetPrimaryAssetIdList(FPrimaryAssetType(TEXT("Project")), Ids);
	for (const FPrimaryAssetId& Id : Ids)
	{
		UProjectDefinitionDataAsset* Def = Cast<UProjectDefinitionDataAsset>(
			UAssetManager::Get().GetPrimaryAssetObject(Id));
		if (!Def)
		{
			TSoftObjectPtr<UProjectDefinitionDataAsset> Soft(UAssetManager::Get().GetPrimaryAssetPath(Id));
			Def = Soft.LoadSynchronous();
		}
		if (!Def)
		{
			continue;
		}
		FProjectRuntime Runtime;
		Runtime.ProjectId = Def->ProjectId;
		Runtime.State = EProjectState::Available;
		GS->Projects.Add(Runtime);
	}
}

bool UProjectSubsystem::AcceptProject(FName ProjectId)
{
	AExoneerGameState* GS = GetGS();
	if (!GS || !GS->HasAuthority())
	{
		return false;
	}
	EnsureCatalog();
	UWorld* World = GetWorld();
	APlanetEnvironmentManager* Env = nullptr;
	if (World)
	{
		for (TActorIterator<APlanetEnvironmentManager> It(World); It; ++It)
		{
			Env = *It;
			break;
		}
	}
	for (FProjectRuntime& Runtime : GS->Projects)
	{
		if (Runtime.ProjectId != ProjectId)
		{
			continue;
		}
		if (Runtime.State == EProjectState::Active)
		{
			return true;
		}
		Runtime.State = EProjectState::Active;
		Runtime.StartedTimeOfDay01 = Env ? Env->TimeOfDay01 : 0.f;
		Runtime.StartedSol = GS->SolIndex;
		Runtime.SolsActive = 0;
		Runtime.bSawStorm = false;
		Runtime.LastFailReason.Reset();
		return true;
	}
	return false;
}

bool UProjectSubsystem::AbandonProject(FName ProjectId)
{
	AExoneerGameState* GS = GetGS();
	if (!GS || !GS->HasAuthority())
	{
		return false;
	}
	for (FProjectRuntime& Runtime : GS->Projects)
	{
		if (Runtime.ProjectId == ProjectId && Runtime.State == EProjectState::Active)
		{
			Runtime.State = EProjectState::Abandoned;
			return true;
		}
	}
	return false;
}

void UProjectSubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_Client)
	{
		return;
	}
	EnsureCatalog();
	AExoneerGameState* GS = GetGS();
	APlanetEnvironmentManager* Env = nullptr;
	for (TActorIterator<APlanetEnvironmentManager> It(World); It; ++It)
	{
		Env = *It;
		break;
	}
	if (GS && Env)
	{
		if (Env->TimeOfDay01 + 0.05f < LastTimeOfDay01)
		{
			++GS->SolIndex;
		}
		LastTimeOfDay01 = Env->TimeOfDay01;
	}

	EvalAccumulator += DeltaTime;
	if (EvalAccumulator < 1.f)
	{
		return;
	}
	EvalAccumulator = 0.f;
	EvaluateActive();
}

void UProjectSubsystem::EvaluateActive()
{
	AExoneerGameState* GS = GetGS();
	UWorld* World = GetWorld();
	if (!GS || !World)
	{
		return;
	}
	APlanetEnvironmentManager* Env = nullptr;
	for (TActorIterator<APlanetEnvironmentManager> It(World); It; ++It)
	{
		Env = *It;
		break;
	}

	for (FProjectRuntime& Runtime : GS->Projects)
	{
		if (Runtime.State != EProjectState::Active)
		{
			continue;
		}
		if (Env && Env->IsStormActive())
		{
			Runtime.bSawStorm = true;
		}
		Runtime.SolsActive = GS->SolIndex - Runtime.StartedSol;
		const UProjectDefinitionDataAsset* Def = FindDef(Runtime.ProjectId);
		if (!Def)
		{
			continue;
		}
		FString Fail;
		if (EvaluateProject(Def, Runtime, Fail))
		{
			Runtime.State = EProjectState::Succeeded;
			Runtime.LastFailReason.Reset();
			if (Def->bGrantsOrbitalKnowledge)
			{
				GS->Orbital.bHasHandshake = true;
				GS->Orbital.NextWindowTimeOfDay01 = Env ? FMath::Fmod(Env->TimeOfDay01 + 0.15f, 1.f) : 0.5f;
			}
		}
		else if (Def->DurationSols > 0 && Runtime.SolsActive >= Def->DurationSols && !Fail.IsEmpty())
		{
			Runtime.State = EProjectState::Failed;
			Runtime.LastFailReason = Fail;
		}
		else if (!Fail.IsEmpty())
		{
			Runtime.LastFailReason = Fail;
		}
	}
}

bool UProjectSubsystem::EvaluateProject(const UProjectDefinitionDataAsset* Def, FProjectRuntime& Runtime, FString& OutFail) const
{
	if (Def->DurationSols > 0 && Runtime.SolsActive < Def->DurationSols)
	{
		OutFail = FString::Printf(TEXT("hold %d/%d sols"), Runtime.SolsActive, Def->DurationSols);
		return false;
	}
	for (const FProjectCriterion& Criterion : Def->Criteria)
	{
		FString Fail;
		if (!CheckCriterion(Criterion.Type, Criterion.Target, Runtime, Fail))
		{
			OutFail = Fail;
			return false;
		}
	}
	return true;
}

bool UProjectSubsystem::CheckCriterion(EProjectCriterionType Type, float Target, const FProjectRuntime& Runtime, FString& OutFail) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		OutFail = TEXT("no world");
		return false;
	}

	switch (Type)
	{
	case EProjectCriterionType::PowerReserveHours:
	{
		float Stored = 0.f;
		float Demand = 0.f;
		for (TActorIterator<ABaseStructure> It(World); It; ++It)
		{
			if (UPowerNetworkComponent* Net = It->PowerNetwork)
			{
				const FPowerNetworkSnapshot& Snap = Net->GetSnapshot();
				Stored += Snap.TotalStored;
				Demand += Snap.TotalDemand;
			}
		}
		const float Hours = (Demand > 1.f) ? (Stored / Demand) / 3600.f : (Stored > 0.f ? 99.f : 0.f);
		if (Hours < Target)
		{
			OutFail = FString::Printf(TEXT("power reserve %.2f h < %.2f h"), Hours, Target);
			return false;
		}
		return true;
	}
	case EProjectCriterionType::OxygenReserveHours:
	{
		float Stored = 0.f;
		for (TActorIterator<APlayerSurvivalCharacter> It(World); It; ++It)
		{
			if (It->Survival)
			{
				Stored = FMath::Max(Stored, It->Survival->GetOxygenNormalized());
			}
		}
		if (Stored < Target)
		{
			OutFail = FString::Printf(TEXT("suit O2 %.0f%% below target"), Stored * 100.f);
			return false;
		}
		return true;
	}
	case EProjectCriterionType::CommsHops:
	{
		int32 Hops = 0;
		for (TActorIterator<ABasePiece> It(World); It; ++It)
		{
			if (!It->IsFunctional() || !It->Def)
			{
				continue;
			}
			const FName Id = It->Def->PieceId;
			if (Id == TEXT("radio_mast") || Id == TEXT("dish_array"))
			{
				++Hops;
			}
		}
		if (Hops < static_cast<int32>(Target))
		{
			OutFail = FString::Printf(TEXT("comms hops %d < %.0f"), Hops, Target);
			return false;
		}
		return true;
	}
	case EProjectCriterionType::StormSurvived:
	{
		if (Target > 0.f && !Runtime.bSawStorm)
		{
			OutFail = TEXT("no storm during watch");
			return false;
		}
		return true;
	}
	case EProjectCriterionType::DishComplete:
	{
		for (TActorIterator<ABasePiece> It(World); It; ++It)
		{
			if (It->IsFunctional() && It->Def && It->Def->PieceId == TEXT("dish_array"))
			{
				return true;
			}
		}
		OutFail = TEXT("no complete dish");
		return false;
	}
	case EProjectCriterionType::PadPowered:
	{
		for (TActorIterator<ABaseStructure> It(World); It; ++It)
		{
			if (It->PowerNetwork && It->PowerNetwork->GetSnapshot().TotalProduction > Target)
			{
				return true;
			}
		}
		OutFail = FString::Printf(TEXT("pad production below %.0f W"), Target);
		return false;
	}
	case EProjectCriterionType::FuelMassKg:
	{
		float Fuel = 0.f;
		for (TActorIterator<AVehicleConstruct> It(World); It; ++It)
		{
			Fuel += It->GetStoredFuelKg();
		}
		if (Fuel < Target)
		{
			OutFail = FString::Printf(TEXT("fuel %.1f kg < %.1f kg"), Fuel, Target);
			return false;
		}
		return true;
	}
	case EProjectCriterionType::AscentTwr:
	{
		for (TActorIterator<AVehicleConstruct> It(World); It; ++It)
		{
			if (It->GetAscentTwr() >= Target)
			{
				return true;
			}
		}
		OutFail = FString::Printf(TEXT("no stack with TWR >= %.2f"), Target);
		return false;
	}
	default:
		return true;
	}
}
