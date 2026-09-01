// Copyright Exoneer contributors.
#include "World/TestRoverSpawner.h"
#include "Exoneer.h"
#include "Vehicles/VehicleConstruct.h"
#include "Vehicles/PilotInput.h"
#include "Vehicles/VehicleModule.h"
#include "Vehicles/VehicleOrientation.h"
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
	UVehicleBlockDefinitionDataAsset* WheelSteerDef = LoadBlockDef(TEXT("DA_Block_WheelRoadSteer"));
	UVehicleBlockDefinitionDataAsset* WheelDriveDef = LoadBlockDef(TEXT("DA_Block_WheelRoadDrive"));
	UVehicleBlockDefinitionDataAsset* ThrusterDef = LoadBlockDef(TEXT("DA_Block_Thruster"));
	UVehicleBlockDefinitionDataAsset* GyroDef = LoadBlockDef(TEXT("DA_Block_Gyro"));
	if (!FrameDef || !CockpitDef || !BatteryDef || !SolarDef || !WheelSteerDef || !WheelDriveDef
		|| !ThrusterDef || !GyroDef)
	{
		UE_LOG(LogExoneer, Warning, TEXT("TestRoverSpawner: vehicle block definitions missing (run the bootstrap); no rover spawned."));
		return;
	}

	struct FPlacement
	{
		UVehicleBlockDefinitionDataAsset* Def;
		FIntVector Origin;
		float StateScalar;
		uint8 Orientation = 0;
	};
	TArray<FPlacement> Placements;

	// Thruster aim indices come from the SAME helper the build tool's aim list
	// uses, so a spawned and a hand-built rover can never disagree about which
	// orientation means "thrust up".
	const uint8 ThrustUp = ExoneerVehicleOrientation::FindOrientationMappingAxis(
		ExoneerThruster::LocalThrustAxis, FVector::UpVector);
	const uint8 ThrustForward = ExoneerVehicleOrientation::FindOrientationMappingAxis(
		ExoneerThruster::LocalThrustAxis, FVector::ForwardVector);

	// --- Ladder chassis, 12 x 4 cells (3.00 m x 1.00 m), Z = 0 ---
	// A ladder, not a slab: 31 frames instead of 60 keeps the rover near 3 t
	// instead of 4.5 t. Order matters - the spawner places in one pass, so
	// every block must already touch something placed.
	// Left longeron (Y = 0). The founder occupies (0,0,0).
	for (int32 X = 1; X <= 11; ++X)
	{
		Placements.Add({ FrameDef, FIntVector(X, 0, 0), 0.f });
	}
	// Rear crossmember first: it is the only bridge across to the right rail.
	Placements.Add({ FrameDef, FIntVector(1, 1, 0), 0.f });
	Placements.Add({ FrameDef, FIntVector(1, 2, 0), 0.f });
	// Right longeron (Y = 3), starting at the cell that touches the bridge.
	for (int32 X = 1; X <= 11; ++X)
	{
		Placements.Add({ FrameDef, FIntVector(X, 3, 0), 0.f });
	}
	Placements.Add({ FrameDef, FIntVector(0, 3, 0), 0.f });
	// Remaining crossmembers: mid axle, front axle, nose.
	for (const int32 X : { 5, 9, 11 })
	{
		Placements.Add({ FrameDef, FIntVector(X, 1, 0), 0.f });
		Placements.Add({ FrameDef, FIntVector(X, 2, 0), 0.f });
	}

	// --- Wheels: three axles at 1.00 m spacing, track 1.25 m ---
	// Each is 3x1x3 cells with Origin.Z = -1, so it spans Z -1..1 and its
	// inboard column touches the rail row at Z = 0. Axle is block local Y, so
	// orientation 0 is correct.
	// Only the FRONT pair steers: ServerRouteDrive derives the Ackermann turn
	// centre from the centroid of the NON-steered wheels, which is the one
	// split that solver resolves correctly (a steered rear pair would crab).
	// All six are driven (the steer spec keeps bDriven), so this is 6x6.
	Placements.Add({ WheelDriveDef, FIntVector(0, -1, -1), 0.f });
	Placements.Add({ WheelDriveDef, FIntVector(0,  4, -1), 0.f });
	Placements.Add({ WheelDriveDef, FIntVector(4, -1, -1), 0.f });
	Placements.Add({ WheelDriveDef, FIntVector(4,  4, -1), 0.f });
	Placements.Add({ WheelSteerDef, FIntVector(8, -1, -1), 0.f });
	Placements.Add({ WheelSteerDef, FIntVector(8,  4, -1), 0.f });

	// --- Module bay down the centre lane (Y = 1..2), Z = 1 ---
	// Battery bank over the rear axle, delivered full (StateScalar 1).
	Placements.Add({ BatteryDef, FIntVector(1, 1, 1), 1.f });
	Placements.Add({ BatteryDef, FIntVector(1, 2, 1), 1.f });
	Placements.Add({ BatteryDef, FIntVector(2, 1, 1), 1.f });
	Placements.Add({ BatteryDef, FIntVector(2, 2, 1), 1.f });
	// Attitude gyro (2x2x2: X 5..6, Y 1..2, Z 1..2) on the mid crossmember.
	// Must precede the solar pair, which anchors to its cells.
	Placements.Add({ GyroDef, FIntVector(5, 1, 1), 0.f });
	// Second gyro (X 7..8): one unit is marginal in yaw on a ~2 t hull, two
	// give the pilot real authority and let attitude hold actually settle it.
	Placements.Add({ GyroDef, FIntVector(7, 1, 1), 0.f });
	Placements.Add({ SolarDef, FIntVector(4, 1, 1), 0.f });
	Placements.Add({ SolarDef, FIntVector(4, 2, 1), 0.f });
	Placements.Add({ CockpitDef, FIntVector(11, 1, 1), 0.f });

	// --- Thrusters ---
	// Six lifting, on the outer rails at X = 2 / 5 / 8. Their centroid lands
	// on the centre of mass, so full lift is pure force with no pitch or roll
	// trim - that is why the solar pair sits at X = 4 rather than the nose.
	for (const int32 X : { 2, 5, 8 })
	{
		Placements.Add({ ThrusterDef, FIntVector(X, 0, 1), 0.f, ThrustUp });
		Placements.Add({ ThrusterDef, FIntVector(X, 3, 1), 0.f, ThrustUp });
	}
	// Two facing forward, in the open rear crossmember cells at Z = 0 -
	// BELOW the CoM on purpose, so forward thrust pitches the nose UP, which
	// tilts the lift vector back and self-limits. Mounted above the CoM the
	// same pair would pitch nose-down and the coupling diverges.
	Placements.Add({ ThrusterDef, FIntVector(0, 1, 0), 0.f, ThrustForward });
	Placements.Add({ ThrusterDef, FIntVector(0, 2, 0), 0.f, ThrustForward });

	// Centre the rover on the garage pad. The actor pivot is the chassis
	// rear-left-bottom corner, so a rover founded at the spawner would grow
	// 3 m in +X and sit visibly off-centre. Derived from the cells actually
	// placed, so editing the layout above cannot silently push it off the pad.
	FIntVector MinCell = FIntVector::ZeroValue;
	FIntVector MaxCell = FIntVector::ZeroValue;
	TArray<FIntVector> Cells;
	for (const FPlacement& Placement : Placements)
	{
		ExoneerVehicleOrientation::GetOccupiedCells(Placement.Origin, Placement.Def->SizeInCells, Placement.Orientation, Cells);
		for (const FIntVector& Cell : Cells)
		{
			MinCell = FIntVector(FMath::Min(MinCell.X, Cell.X), FMath::Min(MinCell.Y, Cell.Y), FMath::Min(MinCell.Z, Cell.Z));
			MaxCell = FIntVector(FMath::Max(MaxCell.X, Cell.X), FMath::Max(MaxCell.Y, Cell.Y), FMath::Max(MaxCell.Z, Cell.Z));
		}
	}
	const FVector LocalCentre(
		(MinCell.X + MaxCell.X + 1) * 0.5f * AVehicleConstruct::CellSize,
		(MinCell.Y + MaxCell.Y + 1) * 0.5f * AVehicleConstruct::CellSize,
		0.f);

	FTransform SpawnTransform = GetActorTransform();
	SpawnTransform.SetScale3D(FVector::OneVector);
	SpawnTransform.SetLocation(GetActorLocation()
		- GetActorQuat().RotateVector(LocalCentre)
		+ FVector(0.f, 0.f, SpawnHeightUU));

	EBuildPlacementError Error = EBuildPlacementError::None;
	AVehicleConstruct* Rover = AVehicleConstruct::FoundConstruct(World, FrameDef, SpawnTransform, Error);
	if (!Rover || Rover->GetBlockCount() == 0)
	{
		UE_LOG(LogExoneer, Warning, TEXT("TestRoverSpawner: FoundConstruct failed (error %d)."), (int32)Error);
		return;
	}

	TArray<int32> PlacedIds;
	PlacedIds.Add(Rover->GetBlocks()[0].BlockInstanceId);
	TArray<const UVehicleBlockDefinitionDataAsset*> PlacedDefs;
	PlacedDefs.Add(FrameDef);
	TArray<float> PlacedScalars;
	PlacedScalars.Add(0.f);

	for (const FPlacement& Placement : Placements)
	{
		const int32 BlockId = Rover->PlaceBlockGhost(Placement.Def, Placement.Origin, Placement.Orientation);
		if (BlockId == INDEX_NONE)
		{
			// An Error, not a Warning: with 50+ placements one bad cell ships a
			// silently crippled rover.
			UE_LOG(LogExoneer, Error, TEXT("TestRoverSpawner: could not place %s at (%d,%d,%d)."),
				*Placement.Def->BlockId.ToString(), Placement.Origin.X, Placement.Origin.Y, Placement.Origin.Z);
			continue;
		}
		PlacedIds.Add(BlockId);
		PlacedDefs.Add(Placement.Def);
		PlacedScalars.Add(Placement.StateScalar);
	}
	if (PlacedIds.Num() != Placements.Num() + 1)
	{
		UE_LOG(LogExoneer, Error, TEXT("TestRoverSpawner: %d of %d blocks failed to place - the layout is wrong."),
			Placements.Num() + 1 - PlacedIds.Num(), Placements.Num() + 1);
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
