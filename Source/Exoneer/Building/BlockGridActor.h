// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/BuildToolComponent.h"   // EBuildPlacementError
#include "BlockGridActor.generated.h"

class UBlockDefinitionDataAsset;
class ABuildableBlock;
class UPowerNetworkComponent;

/**
 * Owns a 3D dictionary of placed blocks for one structure (a base, a vehicle,
 * or a station). Handles cell <-> world conversions, placement validation,
 * spawning, removal, and hosts a UPowerNetworkComponent that ticks the grid's
 * power simulation.
 */
UCLASS(BlueprintType)
class EXONEER_API ABlockGridActor : public AActor
{
	GENERATED_BODY()

public:
	ABlockGridActor();

	/** Edge length, in cm, of a single small-grid cell. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Grid") float CellSize = 100.f;

	/** True for vehicle/ship grids — affects which blocks are accepted. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Grid") bool bIsVehicleGrid = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) UPowerNetworkComponent* PowerNetwork = nullptr;

	UFUNCTION(BlueprintPure, Category = "Grid") FIntVector WorldToCell(const FVector& WorldLocation) const;
	UFUNCTION(BlueprintPure, Category = "Grid") FVector CellToWorld(const FIntVector& Cell) const;
	UFUNCTION(BlueprintPure, Category = "Grid") FTransform CellToWorldTransform(const FIntVector& Cell, int32 RotationStep) const;

	UFUNCTION(BlueprintCallable, Category = "Grid")
	bool CanPlaceBlock(UBlockDefinitionDataAsset* Def, const FIntVector& Cell, int32 RotationStep, EBuildPlacementError& OutError) const;

	UFUNCTION(BlueprintCallable, Category = "Grid")
	ABuildableBlock* PlaceBlock(UBlockDefinitionDataAsset* Def, const FIntVector& Cell, int32 RotationStep);

	UFUNCTION(BlueprintCallable, Category = "Grid")
	bool RemoveBlockAt(const FIntVector& Cell);

	UFUNCTION(BlueprintPure, Category = "Grid")
	ABuildableBlock* GetBlockAt(const FIntVector& Cell) const;

	UFUNCTION(BlueprintPure, Category = "Grid")
	int32 GetBlockCount() const { return Blocks.Num(); }

	UFUNCTION(BlueprintPure, Category = "Grid")
	float GetTotalMass() const;

	UFUNCTION(BlueprintPure, Category = "Grid")
	const TMap<FIntVector, ABuildableBlock*>& GetBlocks() const { return Blocks; }

protected:
	UPROPERTY() TMap<FIntVector, ABuildableBlock*> Blocks;

	bool CellsOccupied(const FIntVector& Origin, const FIntVector& Size) const;
};
