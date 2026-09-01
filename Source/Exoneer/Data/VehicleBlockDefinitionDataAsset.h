// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ExoneerTypes.h"
#include "Vehicles/VehicleModule.h"   // TSubclassOf<UVehicleModule> needs the complete type
#include "Vehicles/VehicleWheelSpec.h"
#include "VehicleBlockDefinitionDataAsset.generated.h"

class UStaticMesh;
class UTexture2D;

/**
 * Data-driven description of one vehicle block on the unified 25 cm grid.
 * Structural blocks have no ModuleClass; functional blocks (thruster, cockpit,
 * battery, solar) name a UVehicleModule subclass that the construct
 * instantiates server-side. Author instances under
 * /Content/Exoneer/Data/VehicleBlocks/.
 */
UCLASS(BlueprintType)
class EXONEER_API UVehicleBlockDefinitionDataAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Stable identifier ("frame_1x1", "thruster_small", "cockpit", ...). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AssetRegistrySearchable, Category = "Block")
	FName BlockId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block")
	FText DisplayName;

	/** AABB occupancy in 25 cm cells, before orientation. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block", meta = (ClampMin = "1"))
	FIntVector SizeInCells = FIntVector(1, 1, 1);

	/** Kilograms; contributes to the construct's rigid body mass and COM. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Stats")
	float Mass = 50.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Stats")
	float MaxHealth = 150.f;

	/** Ghost -> complete investment stages (materials + weld work). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Construction")
	TArray<FConstructionCost> Stages;

	/** Null for structural blocks. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	TSubclassOf<UVehicleModule> ModuleClass;

	/** Watts. Positive produces, negative consumes at full activity. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float PowerDelta = 0.f;

	/** Watt-seconds a battery block stores (0 for non-batteries). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float EnergyStorage = 0.f;

	/** Newtons at full throttle (thruster blocks). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float MaxThrust = 0.f;

	/**
	 * Rated reaction torque per axis (N*m) for attitude gyro blocks. The
	 * construct sums this over Complete gyros; a vehicle with none has no
	 * attitude authority at all, which is the physical truth.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float MaxGyroTorqueNm = 0.f;

	/** True for wheel blocks: excluded from ISMC visuals, gets a UWheelModule + a dedicated animated mesh component. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel")
	bool bIsWheel = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel", meta = (EditCondition = "bIsWheel"))
	FVehicleWheelSpec WheelSpec;

	/**
	 * Skip the world-static overlap rejection when placing this block: wheels
	 * sit at ground level by design and would otherwise be unplaceable on a
	 * hull resting on terrain.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block")
	bool bAllowTerrainOverlapOnPlace = false;

	/**
	 * Applied between the block's local frame and the mesh, before the
	 * cell-fit scale. Lets the Z-aligned engine cylinder stand in for a
	 * Y-axis wheel (roll 90).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Visual")
	FTransform MeshRelativeTransform = FTransform::Identity;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Visual")
	TSoftObjectPtr<UStaticMesh> Mesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Visual")
	TSoftObjectPtr<UTexture2D> Icon;

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId(TEXT("VehicleBlock"), BlockId);
	}
};
