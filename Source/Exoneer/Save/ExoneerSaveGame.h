// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "Components/InventoryComponent.h"
#include "ExoneerSaveGame.generated.h"

USTRUCT(BlueprintType)
struct FSavedBlock
{
	GENERATED_BODY()

	UPROPERTY() FName BlockId;
	UPROPERTY() FIntVector Cell = FIntVector::ZeroValue;
	UPROPERTY() int32 RotationStep = 0;
	UPROPERTY() float Health = 100.f;
	UPROPERTY() TArray<FInventoryEntry> Inventory;
	UPROPERTY() float StoredEnergy = 0.f;
	UPROPERTY() float StoredOxygen = 0.f;
};

USTRUCT(BlueprintType)
struct FSavedGrid
{
	GENERATED_BODY()

	UPROPERTY() FTransform GridTransform;
	UPROPERTY() bool bIsVehicle = false;
	UPROPERTY() TArray<FSavedBlock> Blocks;
};

UCLASS(BlueprintType)
class EXONEER_API UExoneerSaveGame : public USaveGame
{
	GENERATED_BODY()
public:
	UPROPERTY() FString SlotName = TEXT("ExoneerDefault");
	UPROPERTY() FTransform PlayerTransform;
	UPROPERTY() TArray<FInventoryEntry> PlayerInventory;

	UPROPERTY() float PlayerHealth = 100.f;
	UPROPERTY() float PlayerOxygen = 100.f;
	UPROPERTY() float PlayerSuitPower = 100.f;
	UPROPERTY() float PlayerNutrition = 100.f;
	UPROPERTY() float PlayerBodyTempC = 36.6f;

	UPROPERTY() float TimeOfDay = 0.25f;
	UPROPERTY() float TotalElapsedSeconds = 0.f;

	UPROPERTY() TArray<FSavedGrid> Grids;
};
