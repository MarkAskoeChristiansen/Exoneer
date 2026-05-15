// Copyright Exoneer contributors.
#include "Save/SaveGameSubsystem.h"
#include "Save/ExoneerSaveGame.h"
#include "Building/BlockGridActor.h"
#include "Building/BuildableBlock.h"
#include "Vehicles/VehicleGridActor.h"
#include "Player/PlayerSurvivalCharacter.h"
#include "World/PlanetEnvironmentManager.h"
#include "Data/BlockDefinitionDataAsset.h"
#include "Components/InventoryComponent.h"
#include "Components/PowerComponent.h"
#include "Components/OxygenComponent.h"
#include "Components/SurvivalStatsComponent.h"
#include "Components/HealthComponent.h"
#include "Engine/AssetManager.h"
#include "Kismet/GameplayStatics.h"
#include "EngineUtils.h"

bool USaveGameSubsystem::HasSave(const FString& SlotName) const
{
	return UGameplayStatics::DoesSaveGameExist(SlotName, 0);
}

bool USaveGameSubsystem::SaveToSlot(const FString& SlotName)
{
	UExoneerSaveGame* Save = GatherWorldState();
	if (!Save) return false;
	Save->SlotName = SlotName;
	return UGameplayStatics::SaveGameToSlot(Save, SlotName, 0);
}

bool USaveGameSubsystem::LoadFromSlot(const FString& SlotName)
{
	if (!HasSave(SlotName)) return false;
	USaveGame* Loaded = UGameplayStatics::LoadGameFromSlot(SlotName, 0);
	UExoneerSaveGame* Save = Cast<UExoneerSaveGame>(Loaded);
	if (!Save) return false;
	ApplyWorldState(Save);
	return true;
}

UExoneerSaveGame* USaveGameSubsystem::GatherWorldState() const
{
	UWorld* World = GetWorld();
	if (!World) return nullptr;

	UExoneerSaveGame* Save = Cast<UExoneerSaveGame>(UGameplayStatics::CreateSaveGameObject(UExoneerSaveGame::StaticClass()));
	if (!Save) return nullptr;

	// Player.
	if (APlayerSurvivalCharacter* P = Cast<APlayerSurvivalCharacter>(UGameplayStatics::GetPlayerPawn(World, 0)))
	{
		Save->PlayerTransform = P->GetActorTransform();
		if (P->Inventory)
		{
			Save->PlayerInventory = P->Inventory->GetEntries();
		}
		if (P->HealthC)   Save->PlayerHealth     = P->HealthC->Health;
		if (P->Survival)
		{
			Save->PlayerOxygen      = P->Survival->Oxygen;
			Save->PlayerSuitPower   = P->Survival->SuitPower;
			Save->PlayerNutrition   = P->Survival->Nutrition;
			Save->PlayerBodyTempC   = P->Survival->GetBodyTemperature();
		}
	}

	// Environment.
	for (TActorIterator<APlanetEnvironmentManager> It(World); It; ++It)
	{
		Save->TimeOfDay = It->TimeOfDay;
		break;
	}

	// Grids (base + vehicles).
	for (TActorIterator<ABlockGridActor> It(World); It; ++It)
	{
		FSavedGrid Grid;
		Grid.GridTransform = It->GetActorTransform();
		Grid.bIsVehicle = It->IsA<AVehicleGridActor>();

		TSet<ABuildableBlock*> Visited;
		for (const auto& KV : It->GetBlocks())
		{
			if (Visited.Contains(KV.Value)) continue;
			Visited.Add(KV.Value);
			if (!KV.Value || !KV.Value->Definition) continue;
			FSavedBlock B;
			B.BlockId = KV.Value->Definition->BlockId;
			B.Cell = KV.Value->GetGridCoord();
			B.RotationStep = KV.Value->RotationStep;
			B.Health = KV.Value->Health;
			if (UInventoryComponent* I = KV.Value->FindComponentByClass<UInventoryComponent>())
			{
				B.Inventory = I->GetEntries();
			}
			if (UPowerComponent* PC = KV.Value->FindComponentByClass<UPowerComponent>())
			{
				B.StoredEnergy = PC->StoredEnergy;
			}
			if (UOxygenComponent* OC = KV.Value->FindComponentByClass<UOxygenComponent>())
			{
				B.StoredOxygen = OC->Stored;
			}
			Grid.Blocks.Add(B);
		}
		Save->Grids.Add(Grid);
	}

	return Save;
}

void USaveGameSubsystem::ApplyWorldState(UExoneerSaveGame* Save)
{
	UWorld* World = GetWorld();
	if (!World || !Save) return;

	// Player.
	if (APlayerSurvivalCharacter* P = Cast<APlayerSurvivalCharacter>(UGameplayStatics::GetPlayerPawn(World, 0)))
	{
		P->SetActorTransform(Save->PlayerTransform);
		if (P->Inventory)
		{
			// Naive reset: clear and re-add. (A real impl would expose a setter.)
			TArray<FInventoryEntry> Existing = P->Inventory->GetEntries();
			for (const FInventoryEntry& E : Existing)
			{
				P->Inventory->RemoveItem(E.Item.LoadSynchronous(), E.Count);
			}
			for (const FInventoryEntry& E : Save->PlayerInventory)
			{
				P->Inventory->AddItem(E.Item.LoadSynchronous(), E.Count);
			}
		}
		if (P->Survival)
		{
			P->Survival->Oxygen     = Save->PlayerOxygen;
			P->Survival->SuitPower  = Save->PlayerSuitPower;
			P->Survival->Nutrition  = Save->PlayerNutrition;
		}
	}

	for (TActorIterator<APlanetEnvironmentManager> It(World); It; ++It)
	{
		It->TimeOfDay = Save->TimeOfDay;
		break;
	}

	// NOTE: Re-spawning grids/blocks requires a resolved BlockId->UBlockDefinitionDataAsset
	// lookup. The expected flow is for the GameInstance to maintain a cached
	// PrimaryAssetId map; see README "Save/Load" section for the recommended
	// integration approach.
}
