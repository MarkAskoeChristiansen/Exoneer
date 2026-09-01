// Copyright Exoneer contributors.
#include "World/TestRoverSpawner.h"
#include "Exoneer.h"
#include "Vehicles/VehicleConstruct.h"
#include "Vehicles/PilotInput.h"
#include "Data/VehicleBlockDefinitionDataAsset.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace
{
	UVehicleBlockDefinitionDataAsset* LoadBlockDef(const TCHAR* AssetName)
	{
		const FString Path = FString::Printf(TEXT("/Game/Exoneer/Data/VehicleBlocks/%s.%s"), AssetName, AssetName);
		return LoadObject<UVehicleBlockDefinitionDataAsset>(nullptr, *Path);
	}
}

ATestRoverSpawner::ATestRoverSpawner()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void ATestRoverSpawner::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority())
	{
		SpawnRover();
	}
}

void ATestRoverSpawner::SpawnRover()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// A rover from an earlier session or a loaded save takes precedence.
	for (TActorIterator<AVehicleConstruct> It(World); It; ++It)
	{
		if (FVector::Dist(It->GetActorLocation(), GetActorLocation()) < ExistingConstructCheckRadius)
		{
			return;
		}
	}

	UVehicleBlockDefinitionDataAsset* FrameDef = LoadBlockDef(TEXT("DA_Block_Frame"));
	UVehicleBlockDefinitionDataAsset* CockpitDef = LoadBlockDef(TEXT("DA_Block_Cockpit"));
	UVehicleBlockDefinitionDataAsset* BatteryDef = LoadBlockDef(TEXT("DA_Block_Battery"));
	UVehicleBlockDefinitionDataAsset* SolarDef = LoadBlockDef(TEXT("DA_Block_Solar"));
	UVehicleBlockDefinitionDataAsset* WheelSteerDef = LoadBlockDef(TEXT("DA_Block_WheelSteer"));
	UVehicleBlockDefinitionDataAsset* WheelDriveDef = LoadBlockDef(TEXT("DA_Block_WheelDrive"));
	if (!FrameDef || !CockpitDef || !BatteryDef || !SolarDef || !WheelSteerDef || !WheelDriveDef)
	{
		UE_LOG(LogExoneer, Warning, TEXT("TestRoverSpawner: vehicle block definitions missing (run the bootstrap); no rover spawned."));
		return;
	}

	// Found on the rear-left deck frame, ~0.9 m up; the suspension settles it.
	FTransform SpawnTransform = GetActorTransform();
	SpawnTransform.SetScale3D(FVector::OneVector);
	SpawnTransform.AddToTranslation(FVector(0.f, 0.f, 90.f));

	EBuildPlacementError Error = EBuildPlacementError::None;
	AVehicleConstruct* Rover = AVehicleConstruct::FoundConstruct(World, FrameDef, SpawnTransform, Error);
	if (!Rover || Rover->GetBlockCount() == 0)
	{
		UE_LOG(LogExoneer, Warning, TEXT("TestRoverSpawner: FoundConstruct failed (error %d)."), (int32)Error);
		return;
	}

	struct FPlacement
	{
		UVehicleBlockDefinitionDataAsset* Def;
		FIntVector Origin;
		float StateScalar;
	};
	TArray<FPlacement> Placements;

	// Deck: 8x3 frames at z = 0 (the founder already sits at 0,0,0).
	for (int32 X = 0; X < 8; ++X)
	{
		for (int32 Y = 0; Y < 3; ++Y)
		{
			if (X == 0 && Y == 0)
			{
				continue;
			}
			Placements.Add({ FrameDef, FIntVector(X, Y, 0), 0.f });
		}
	}
	// Top row: cockpit at the front, charged batteries + solar midship.
	Placements.Add({ CockpitDef, FIntVector(7, 1, 1), 0.f });
	Placements.Add({ BatteryDef, FIntVector(1, 1, 1), 1.f });
	Placements.Add({ BatteryDef, FIntVector(3, 1, 1), 1.f });
	Placements.Add({ SolarDef, FIntVector(2, 1, 1), 0.f });
	// Wheels flanking the deck (3x1x3 cells, vertically centered on it):
	// steering pair front, drive pair rear. Axle = block local Y, so
	// orientation 0 is already correct.
	Placements.Add({ WheelSteerDef, FIntVector(5, -1, -1), 0.f });
	Placements.Add({ WheelSteerDef, FIntVector(5, 3, -1), 0.f });
	Placements.Add({ WheelDriveDef, FIntVector(0, -1, -1), 0.f });
	Placements.Add({ WheelDriveDef, FIntVector(0, 3, -1), 0.f });

	TArray<int32> PlacedIds;
	PlacedIds.Add(Rover->GetBlocks()[0].BlockInstanceId);
	TArray<const UVehicleBlockDefinitionDataAsset*> PlacedDefs;
	PlacedDefs.Add(FrameDef);
	TArray<float> PlacedScalars;
	PlacedScalars.Add(0.f);

	for (const FPlacement& Placement : Placements)
	{
		const int32 BlockId = Rover->PlaceBlockGhost(Placement.Def, Placement.Origin, 0);
		if (BlockId == INDEX_NONE)
		{
			UE_LOG(LogExoneer, Warning, TEXT("TestRoverSpawner: could not place %s at (%d,%d,%d)."),
				*Placement.Def->BlockId.ToString(), Placement.Origin.X, Placement.Origin.Y, Placement.Origin.Z);
			continue;
		}
		PlacedIds.Add(BlockId);
		PlacedDefs.Add(Placement.Def);
		PlacedScalars.Add(Placement.StateScalar);
	}

	// Weld everything to Complete in one pass - this is a delivered vehicle,
	// not a construction exercise.
	for (int32 Index = 0; Index < PlacedIds.Num(); ++Index)
	{
		const UVehicleBlockDefinitionDataAsset* Def = PlacedDefs[Index];
		const int32 LastStage = FMath::Max(Def->Stages.Num() - 1, 0);
		Rover->RestoreBlockRecord(PlacedIds[Index], EConstructionPhase::Complete, LastStage, 1.f, Def->MaxHealth, PlacedScalars[Index]);
	}
	Rover->MarkVisualsDirty();

	// Ready to drive: board with F; W/S drive, A/D steer.
	Rover->ControlMode = EPilotControlMode::Ground;

	UE_LOG(LogExoneer, Log, TEXT("TestRoverSpawner: rover ready (%d blocks)."), Rover->GetBlockCount());
}
