// Copyright Exoneer contributors.
#include "Vehicles/VehicleConstruct.h"
#include "Exoneer.h"
#include "Vehicles/VehicleModule.h"
#include "Vehicles/VehicleOrientation.h"
#include "Vehicles/ExoneerVehicleUnits.h"
#include "Vehicles/ExoneerAttitude.h"
#include "Vehicles/ExoneerThrust.h"
#include "Vehicles/WheelModule.h"
#include "Vehicles/WheelSimCallback.h"
#include "Data/VehicleBlockDefinitionDataAsset.h"
#include "PBDRigidsSolver.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Data/ItemDefinitionDataAsset.h"
#include "Components/InventoryComponent.h"
#include "Components/CargoComponent.h"
#include "Maintenance/ExoneerMaintenance.h"
#include "Engine/AssetManager.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "ExoneerGameplayTags.h"
#include "Player/PlayerSurvivalCharacter.h"
#include "Components/SurvivalStatsComponent.h"
#include "World/PlanetEnvironmentManager.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "Materials/MaterialInterface.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Net/UnrealNetwork.h"

namespace
{
	/** Component tag marking the not-yet-complete visual layers (BPs assign a ghost material). */
	const FName GhostLayerTag(TEXT("ExoneerGhostLayer"));

	/** Component tag marking ghost/under-construction collision boxes. */
	const FName GhostBoxTag(TEXT("ExoneerGhostBox"));

	/** Per-world weak cache for the environment manager; the header adds no member for it. */
	TWeakObjectPtr<UWorld> GSunFractionWorld;
	TWeakObjectPtr<APlanetEnvironmentManager> GSunFractionManager;

	/** Rotated AABB footprint of a record, in whole cells per axis. */
	FIntVector GetRotatedAabbCells(const FVehicleBlockRecord& Record)
	{
		const FIntVector Size = Record.Def ? Record.Def->SizeInCells : FIntVector(1, 1, 1);
		const FIntVector R = ExoneerVehicleOrientation::RotateOffset(Size - FIntVector(1, 1, 1), Record.Orientation);
		return FIntVector(FMath::Abs(R.X) + 1, FMath::Abs(R.Y) + 1, FMath::Abs(R.Z) + 1);
	}

	/** Units of one stage material that must have been consumed at the given progress. */
	int32 UnitsOwedAtProgress(int32 Count, float Progress01)
	{
		return FMath::Clamp(FMath::CeilToInt(Count * Progress01 - KINDA_SMALL_NUMBER), 0, Count);
	}

	bool IsCockpitRecord(const FVehicleBlockRecord& Record)
	{
		return Record.Phase == EConstructionPhase::Complete
			&& Record.Def
			&& Record.Def->ModuleClass
			&& Record.Def->ModuleClass->IsChildOf(UCockpitModule::StaticClass());
	}
}

// --- Fast array callbacks (client-side rebuild triggers) ---

void FVehicleBlockRecord::PreReplicatedRemove(const FVehicleBlockList& InArray)
{
	if (InArray.OwnerConstruct)
	{
		InArray.OwnerConstruct->MarkVisualsDirty();
	}
}

uint32 FVehicleBlockRecord::ComputeVisualKey() const
{
	// Exactly what RebuildDerivedState reads off a record: which definition,
	// where it sits, how it is turned, and whether it is Complete.
	uint32 Key = PointerHash(Def.Get());
	Key = HashCombineFast(Key, GetTypeHash(Origin));
	Key = HashCombineFast(Key, (uint32)Orientation | ((uint32)Phase << 8));
	return Key;
}

void FVehicleBlockRecord::PostReplicatedAdd(const FVehicleBlockList& InArray)
{
	VisualKey = ComputeVisualKey();
	if (InArray.OwnerConstruct)
	{
		InArray.OwnerConstruct->MarkVisualsDirty();
	}
}

void FVehicleBlockRecord::PostReplicatedChange(const FVehicleBlockList& InArray)
{
	// Geometry only. A condition or health update carries no new geometry, and
	// rebuilding the whole construct's visuals for a temperature reading is the
	// cost WheelStates was split off to avoid.
	const uint32 NewKey = ComputeVisualKey();
	if (NewKey == VisualKey)
	{
		return;
	}
	VisualKey = NewKey;
	if (InArray.OwnerConstruct)
	{
		InArray.OwnerConstruct->MarkVisualsDirty();
	}
}

// --- Construction ---

AVehicleConstruct::AVehicleConstruct()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	SetReplicatingMovement(true);

	// Invisible physics root; the welded block boxes carry ALL query and contact
	// collision. The root's own shape must never block traces or pawns on either
	// side, so it starts NoCollision; the server upgrades it to PhysicsOnly with
	// ignore-all responses in BeginPlay (a simulating body needs a shape to
	// exist and to accept welded children, but that shape must stay inert).
	PhysicsRoot = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PhysicsRoot"));
	SetRootComponent(PhysicsRoot);
	PhysicsRoot->SetVisibility(false);
	PhysicsRoot->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PhysicsRoot->SetCollisionResponseToAllChannels(ECR_Ignore);

	BlockList.OwnerConstruct = this;

	Cargo = CreateDefaultSubobject<UCargoComponent>(TEXT("Cargo"));
	Cargo->MaxCapacity = 2000.f;
	Cargo->bUseWeight = false;
}

void AVehicleConstruct::BeginPlay()
{
	Super::BeginPlay();
	BlockList.OwnerConstruct = this;

	if (HasAuthority())
	{
		// The root needs a real body for the block boxes to weld into; give it
		// the engine cube (invisible, near-massless) when no mesh is assigned.
		// PhysicsOnly + ignore-all keeps the 1 m cube shape out of every query
		// and every physical contact - only the welded block boxes collide.
		if (!PhysicsRoot->GetStaticMesh())
		{
			if (UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
			{
				PhysicsRoot->SetStaticMesh(Cube);
			}
		}
		PhysicsRoot->SetCollisionEnabled(ECollisionEnabled::PhysicsOnly);
		PhysicsRoot->SetCollisionResponseToAllChannels(ECR_Ignore);
		PhysicsRoot->BodyInstance.SetMassOverride(1.f, true);
		// Grounded-machine feel: without damping a nudged 25 cm block rolls
		// across the plain forever like it is in orbit. RebuildDerivedState
		// swaps these for the near-zero wheeled values once wheels exist.
		PhysicsRoot->SetLinearDamping(LinearDampingNoWheels);
		PhysicsRoot->SetAngularDamping(AngularDampingNoWheels);
		// Simulation starts in RebuildDerivedState once the construct has at
		// least one COMPLETE block: ghost boxes carry no physical collision,
		// so a freshly founded frame would free-fall through the floor.
	}

	MarkVisualsDirty();
}

void AVehicleConstruct::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AVehicleConstruct, BlockList);
	DOREPLIFETIME(AVehicleConstruct, PowerSupplyFraction);
	DOREPLIFETIME(AVehicleConstruct, PilotPawn);
	DOREPLIFETIME(AVehicleConstruct, ActiveCockpitId);
	DOREPLIFETIME(AVehicleConstruct, ControlMode);
	DOREPLIFETIME(AVehicleConstruct, WheelStates);
	DOREPLIFETIME(AVehicleConstruct, bParkingBrakeEngaged);
	DOREPLIFETIME(AVehicleConstruct, LastLandingSpeedMS);
	DOREPLIFETIME(AVehicleConstruct, GyroSaturation01);
	DOREPLIFETIME(AVehicleConstruct, LiftCollective);
	DOREPLIFETIME(AVehicleConstruct, bLiftGovernorActive);
	DOREPLIFETIME(AVehicleConstruct, UntrimmedStandingMomentNm);
	DOREPLIFETIME(AVehicleConstruct, bLiftGovernorPinned);
	DOREPLIFETIME(AVehicleConstruct, bLiftDescending);
	DOREPLIFETIME(AVehicleConstruct, GyroWorstAxis);
	DOREPLIFETIME(AVehicleConstruct, bLiftInverted);
	DOREPLIFETIME(AVehicleConstruct, StandingSideForceN);
	DOREPLIFETIME(AVehicleConstruct, UntrimmedWorstAxis);
}

// --- Queries ---

const FVehicleBlockRecord* AVehicleConstruct::FindRecord(int32 BlockInstanceId) const
{
	return BlockList.Blocks.FindByPredicate([BlockInstanceId](const FVehicleBlockRecord& R)
	{
		return R.BlockInstanceId == BlockInstanceId;
	});
}

FVehicleBlockRecord* AVehicleConstruct::FindMutableRecord(int32 BlockInstanceId)
{
	return BlockList.Blocks.FindByPredicate([BlockInstanceId](const FVehicleBlockRecord& R)
	{
		return R.BlockInstanceId == BlockInstanceId;
	});
}

void AVehicleConstruct::MarkRecordDirty(FVehicleBlockRecord& Record)
{
	BlockList.MarkItemDirty(Record);
}

bool AVehicleConstruct::RestoreBlockRecord(int32 BlockInstanceId, EConstructionPhase InPhase, int32 InStageIndex, float InStageProgress01, float InHealth, float InStateScalar, int32 OrientationOverride)
{
	if (!HasAuthority())
	{
		UE_LOG(LogExoneer, Warning, TEXT("RestoreBlockRecord called without authority on %s"), *GetName());
		return false;
	}

	FVehicleBlockRecord* Record = FindMutableRecord(BlockInstanceId);
	if (!Record)
	{
		return false;
	}

	bool bOrientationChanged = false;
	if (OrientationOverride >= 0 && OrientationOverride < ExoneerVehicleOrientation::NumOrientations)
	{
		bOrientationChanged = Record->Orientation != static_cast<uint8>(OrientationOverride);
		Record->Orientation = static_cast<uint8>(OrientationOverride);
	}
	Record->StageIndex = FMath::Max(0, InStageIndex);
	Record->StageProgress01 = FMath::Clamp(InStageProgress01, 0.f, 1.f);
	Record->Phase = InPhase;
	const float MaxHealth = Record->Def ? Record->Def->MaxHealth : InHealth;
	Record->Health = FMath::Clamp(InHealth, 0.f, MaxHealth);
	Record->StateScalar = InStateScalar;
	MarkRecordDirty(*Record);

	// Restored Complete blocks need their modules live and the derived state
	// (cells, bodies, visuals) refreshed before the next physics step. An
	// orientation change moves occupied cells, so callers that keep placing
	// blocks afterwards need the cell map rebuilt NOW, not next tick.
	SyncModulesToRecords();
	if (bOrientationChanged)
	{
		RebuildDerivedState();
	}
	else
	{
		MarkVisualsDirty();
	}
	return true;
}

int32 AVehicleConstruct::FindBlockAtCell(const FIntVector& Cell) const
{
	const int32* Found = CellToBlock.Find(Cell);
	return Found ? *Found : INDEX_NONE;
}

FIntVector AVehicleConstruct::WorldToCell(const FVector& WorldLocation) const
{
	const FVector Local = ActorToWorld().InverseTransformPosition(WorldLocation) / CellSize;
	return FIntVector(FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), FMath::FloorToInt32(Local.Z));
}

FVector AVehicleConstruct::CellToWorld(const FIntVector& Cell) const
{
	return ActorToWorld().TransformPosition((FVector(Cell) + FVector(0.5f)) * CellSize);
}

int32 AVehicleConstruct::FindBlockAtWorldPoint(const FVector& WorldPoint) const
{
	const int32 Direct = FindBlockAtCell(WorldToCell(WorldPoint));
	if (Direct != INDEX_NONE)
	{
		return Direct;
	}

	// Fall back to the nearest block center within one cell of its own bounds.
	int32 BestId = INDEX_NONE;
	double BestDistSq = TNumericLimits<double>::Max();
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (!Record.Def)
		{
			continue;
		}
		const FVector Center = GetBlockWorldTransform(Record).GetLocation();
		const double Accept = (FVector(GetRotatedAabbCells(Record)) * (CellSize * 0.5f)).Size() + CellSize;
		const double DistSq = FVector::DistSquared(Center, WorldPoint);
		if (DistSq <= Accept * Accept && DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			BestId = Record.BlockInstanceId;
		}
	}
	return BestId;
}

FTransform AVehicleConstruct::GetBlockLocalTransform(const FVehicleBlockRecord& Record) const
{
	const FIntVector AabbCells = GetRotatedAabbCells(Record);
	const FVector Center = (FVector(Record.Origin) + FVector(AabbCells) * 0.5f) * CellSize;
	return FTransform(ExoneerVehicleOrientation::GetQuat(Record.Orientation), Center);
}

FTransform AVehicleConstruct::GetBlockWorldTransform(const FVehicleBlockRecord& Record) const
{
	return GetBlockLocalTransform(Record) * ActorToWorld();
}

float AVehicleConstruct::GetSunFraction() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return 1.f;
	}
	if (GSunFractionWorld.Get() != World)
	{
		GSunFractionWorld = World;
		GSunFractionManager.Reset();
		for (TActorIterator<APlanetEnvironmentManager> It(World); It; ++It)
		{
			GSunFractionManager = *It;
			break;
		}
	}
	return GSunFractionManager.IsValid() ? GSunFractionManager->GetSunFraction() : 1.f;
}

const APlanetEnvironmentManager* AVehicleConstruct::GetEnvironmentManager() const
{
	// Reuse the sun-fraction cache; GetSunFraction refreshes it per world.
	GetSunFraction();
	return GSunFractionManager.Get();
}

// --- Grid mutations (server) ---

bool AVehicleConstruct::CanPlaceBlock(UVehicleBlockDefinitionDataAsset* Def, FIntVector Origin, uint8 Orientation, EBuildPlacementError& OutError) const
{
	OutError = EBuildPlacementError::None;
	if (!Def)
	{
		OutError = EBuildPlacementError::InvalidDefinition;
		return false;
	}

	TArray<FIntVector> Cells;
	ExoneerVehicleOrientation::GetOccupiedCells(Origin, Def->SizeInCells, Orientation, Cells);

	for (const FIntVector& Cell : Cells)
	{
		if (CellToBlock.Contains(Cell))
		{
			OutError = EBuildPlacementError::CellOccupied;
			return false;
		}
	}

	// World geometry check: the grid only knows THIS construct's cells, so a
	// block could otherwise clip into base pieces, terrain, or other
	// constructs. Shrunk slightly so flush contact does not self-reject.
	// Wheels opt out per definition (they sit at ground level by design);
	// the save-load replay suppresses it construct-wide for byte-exact
	// geometry restoration.
	if (UWorld* World = GetWorld(); World && !Def->bAllowTerrainOverlapOnPlace && !bSuppressWorldOverlapCheck)
	{
		FIntVector MinCell = Cells[0];
		FIntVector MaxCell = Cells[0];
		for (const FIntVector& Cell : Cells)
		{
			MinCell = FIntVector(FMath::Min(MinCell.X, Cell.X), FMath::Min(MinCell.Y, Cell.Y), FMath::Min(MinCell.Z, Cell.Z));
			MaxCell = FIntVector(FMath::Max(MaxCell.X, Cell.X), FMath::Max(MaxCell.Y, Cell.Y), FMath::Max(MaxCell.Z, Cell.Z));
		}
		const FVector LocalCenter = (FVector(MinCell) + FVector(MaxCell) + FVector(1.f)) * 0.5f * CellSize;
		const FVector Extent = (FVector(MaxCell - MinCell) + FVector(1.f)) * (CellSize * 0.5f) * 0.9f;
		const FTransform ActorXf = ActorToWorld();

		FCollisionQueryParams Params(FName(TEXT("ExoneerBlockOverlap")), /*bTraceComplex*/ false);
		Params.AddIgnoredActor(this);
		TArray<FOverlapResult> Overlaps;
		World->OverlapMultiByChannel(Overlaps,
			ActorXf.TransformPosition(LocalCenter), ActorXf.GetRotation(),
			ECC_WorldStatic, FCollisionShape::MakeBox(Extent), Params);
		for (const FOverlapResult& Overlap : Overlaps)
		{
			AActor* Other = Overlap.GetActor();
			if (Other && Other != this && !Other->IsA<APawn>())
			{
				OutError = EBuildPlacementError::BlockedByCollision;
				return false;
			}
		}
	}

	// Any block after the first needs at least one face-adjacent occupied cell.
	if (BlockList.Blocks.Num() > 0)
	{
		static const FIntVector FaceOffsets[6] =
		{
			FIntVector(1, 0, 0), FIntVector(-1, 0, 0),
			FIntVector(0, 1, 0), FIntVector(0, -1, 0),
			FIntVector(0, 0, 1), FIntVector(0, 0, -1)
		};
		bool bAdjacent = false;
		for (const FIntVector& Cell : Cells)
		{
			for (const FIntVector& Offset : FaceOffsets)
			{
				if (CellToBlock.Contains(Cell + Offset))
				{
					bAdjacent = true;
					break;
				}
			}
			if (bAdjacent)
			{
				break;
			}
		}
		if (!bAdjacent)
		{
			OutError = EBuildPlacementError::NotAdjacent;
			return false;
		}
	}
	return true;
}

int32 AVehicleConstruct::PlaceBlockGhost(UVehicleBlockDefinitionDataAsset* Def, FIntVector Origin, uint8 Orientation)
{
	if (!HasAuthority())
	{
		return INDEX_NONE;
	}
	EBuildPlacementError Error = EBuildPlacementError::None;
	if (!CanPlaceBlock(Def, Origin, Orientation, Error))
	{
		return INDEX_NONE;
	}

	FVehicleBlockRecord& Record = BlockList.Blocks.AddDefaulted_GetRef();
	Record.BlockInstanceId = NextBlockInstanceId++;
	Record.Def = Def;
	Record.Origin = Origin;
	Record.Orientation = Orientation;
	Record.Phase = EConstructionPhase::Ghost;
	Record.StageIndex = 0;
	Record.StageProgress01 = 0.f;
	Record.Health = Def->MaxHealth;
	Record.StateScalar = 0.f;
	InitializeRecordCondition(Record);
	BlockList.MarkItemDirty(Record);

	RebuildDerivedState();
	return Record.BlockInstanceId;
}

AVehicleConstruct* AVehicleConstruct::FoundConstruct(UWorld* World, UVehicleBlockDefinitionDataAsset* Def, const FTransform& Transform, EBuildPlacementError& OutError, uint8 Orientation)
{
	OutError = EBuildPlacementError::None;
	if (!World || !Def)
	{
		OutError = EBuildPlacementError::InvalidDefinition;
		return nullptr;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AVehicleConstruct* Construct = World->SpawnActor<AVehicleConstruct>(AVehicleConstruct::StaticClass(), Transform, Params);
	if (!Construct)
	{
		OutError = EBuildPlacementError::Unknown;
		return nullptr;
	}
	if (Construct->PlaceBlockGhost(Def, FIntVector::ZeroValue, Orientation) == INDEX_NONE)
	{
		Construct->Destroy();
		OutError = EBuildPlacementError::Unknown;
		return nullptr;
	}
	return Construct;
}

bool AVehicleConstruct::RemoveBlock(int32 BlockInstanceId)
{
	if (!HasAuthority())
	{
		return false;
	}
	const int32 Index = BlockList.Blocks.IndexOfByPredicate([BlockInstanceId](const FVehicleBlockRecord& R)
	{
		return R.BlockInstanceId == BlockInstanceId;
	});
	if (Index == INDEX_NONE)
	{
		return false;
	}

	if (UVehicleModule* Module = Modules.FindRef(BlockInstanceId))
	{
		Module->Shutdown();
	}
	Modules.Remove(BlockInstanceId);
	BlockList.Blocks.RemoveAt(Index);
	BlockList.MarkArrayDirty();

	RebuildDerivedState();
	RunSplitDetection();
	return true;
}

// --- Damage ---

float AVehicleConstruct::ApplyDamageToBlockAt(const FVector& WorldPoint, float Amount, EExoneerDamageType Type, AActor* DamageInstigator)
{
	if (!HasAuthority() || Amount <= 0.f)
	{
		return 0.f;
	}
	const int32 BlockId = FindBlockAtWorldPoint(WorldPoint);
	FVehicleBlockRecord* Record = FindMutableRecord(BlockId);
	if (!Record)
	{
		return 0.f;
	}

	const float Applied = FMath::Min(Record->Health, Amount);
	Record->Health -= Applied;
	if (Record->Health <= 0.f)
	{
		RemoveBlock(BlockId);
	}
	else
	{
		MarkRecordDirty(*Record);
	}
	return Applied;
}

void AVehicleConstruct::InitializeRecordCondition(FVehicleBlockRecord& Record)
{
	Record.Condition = FPartCondition();
	if (!Record.Def)
	{
		return;
	}
	if (Record.Def->bIsWheel)
	{
		Record.Condition.TreadDepthMm = Record.Def->WheelSpec.NewTreadDepthMm;
		Record.Condition.InflationKPa = Record.Def->WheelSpec.NominalTirePressureKPa;
		// A motor that has never turned sits at ambient, not at a hardcoded
		// 20 C: on a cold planet the readout would otherwise lie for the half
		// minute the thermal step needs to pull it down.
		if (const APlanetEnvironmentManager* Environment = GetEnvironmentManager())
		{
			Record.Condition.WindingTempC = Environment->GetCurrentAmbientTemperatureC();
		}
	}
	if (Record.Def->ModuleClass && Record.Def->ModuleClass->IsChildOf(USolarModule::StaticClass()))
	{
		Record.Condition.SurfaceOpacity01 = 0.f;
	}
	if (Record.Def->FuelCapacityKg > 0.f)
	{
		Record.StateScalar = 0.f; // filled when construction completes
	}
}

bool AVehicleConstruct::RestoreBlockCondition(int32 BlockInstanceId, const FPartCondition& Condition)
{
	if (!HasAuthority())
	{
		return false;
	}
	FVehicleBlockRecord* Record = FindMutableRecord(BlockInstanceId);
	if (!Record)
	{
		return false;
	}
	Record->Condition = Condition;
	MarkRecordDirty(*Record);
	return true;
}

bool AVehicleConstruct::WipePartAt(const FVector& WorldPoint)
{
	if (!HasAuthority())
	{
		return false;
	}
	FVehicleBlockRecord* Record = FindMutableRecord(FindBlockAtWorldPoint(WorldPoint));
	if (!Record || Record->Phase != EConstructionPhase::Complete)
	{
		return false;
	}
	if (Record->Condition.SurfaceOpacity01 <= KINDA_SMALL_NUMBER)
	{
		return false;
	}
	Record->Condition.SurfaceOpacity01 = 0.f;
	MarkRecordDirty(*Record);
	return true;
}

bool AVehicleConstruct::ReplacePartAt(const FVector& WorldPoint, UInventoryComponent* Source)
{
	if (!HasAuthority() || !Source)
	{
		return false;
	}
	FVehicleBlockRecord* Record = FindMutableRecord(FindBlockAtWorldPoint(WorldPoint));
	if (!Record || !Record->Def || Record->Phase != EConstructionPhase::Complete)
	{
		return false;
	}
	// The verb is legal only at a terminal reading (maintenance.md 4): a spare
	// is a fabricated part, not something a press invents for a good tire or a
	// pack with cycles left. Two classes carry a spare at alpha - a tire whose
	// tread is gone, and a battery whose capacity is at the fade floor.
	const FVehicleWheelSpec& Wheel = Record->Def->WheelSpec;
	const bool bTireTerminal = Record->Def->bIsWheel
		&& ExoneerMaintenance::IsTreadTerminal(Record->Condition.TreadDepthMm, Wheel.NewTreadDepthMm);
	const bool bPackTerminal = Record->Def->EnergyStorage > 0.f
		&& ExoneerMaintenance::IsCapacityTerminal(Record->Condition.CapacityFade01);
	if (!bTireTerminal && !bPackTerminal)
	{
		return false;
	}

	// Wheels keep their historical default; anything else must NAME its spare,
	// so widening this path can never quietly charge a battery a tire.
	FName SpareId = Record->Def->SpareItemId;
	if (SpareId.IsNone())
	{
		if (!Record->Def->bIsWheel)
		{
			return false;
		}
		SpareId = FName(TEXT("tire"));
	}
	FPrimaryAssetId SpareAsset(TEXT("Item"), SpareId);
	UItemDefinitionDataAsset* Spare = Cast<UItemDefinitionDataAsset>(UAssetManager::Get().GetPrimaryAssetObject(SpareAsset));
	if (!Spare)
	{
		TSoftObjectPtr<UItemDefinitionDataAsset> Soft(UAssetManager::Get().GetPrimaryAssetPath(SpareAsset));
		Spare = Soft.LoadSynchronous();
	}
	if (!Spare || Source->GetItemCount(Spare) < 1)
	{
		return false;
	}
	if (Source->RemoveItem(Spare, 1) < 1)
	{
		return false;
	}

	// Reset only what the spare covers. A fresh FPartCondition would also drop
	// WindingTempC back to nominal and clear bThermalCutout, so one press with a
	// tire in the pack would repair a cooked motor - and motor replace is
	// post-alpha (alpha gate A2). The winding recovers by cooling, nothing else.
	if (bTireTerminal)
	{
		Record->Condition.TreadDepthMm = Wheel.NewTreadDepthMm;
		Record->Condition.InflationKPa = Wheel.NominalTirePressureKPa;
		Record->Health = Record->Def->MaxHealth;
	}
	if (bPackTerminal)
	{
		// A new cell arrives empty: the joules in the old one left with it.
		Record->Condition.CapacityFade01 = 0.f;
		Record->PendingCapacityFade = 0.f;
		Record->StateScalar = 0.f;
	}
	MarkRecordDirty(*Record);
	return true;
}

bool AVehicleConstruct::HasFuelCapacity() const
{
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (Record.Phase == EConstructionPhase::Complete && Record.Def && Record.Def->FuelCapacityKg > 0.f)
		{
			return true;
		}
	}
	return false;
}

float AVehicleConstruct::GetStoredFuelKg() const
{
	float Total = 0.f;
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (Record.Phase == EConstructionPhase::Complete && Record.Def && Record.Def->FuelCapacityKg > 0.f)
		{
			Total += Record.StateScalar * Record.Def->FuelCapacityKg;
		}
	}
	return Total;
}

bool AVehicleConstruct::ConsumeFuelKg(float Kg)
{
	if (!HasAuthority() || Kg <= 0.f)
	{
		return Kg <= 0.f;
	}
	if (GetStoredFuelKg() + KINDA_SMALL_NUMBER < Kg)
	{
		return false;
	}
	float Remaining = Kg;
	for (FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (Remaining <= 0.f)
		{
			break;
		}
		if (Record.Phase != EConstructionPhase::Complete || !Record.Def || Record.Def->FuelCapacityKg <= 0.f)
		{
			continue;
		}
		const float Stored = Record.StateScalar * Record.Def->FuelCapacityKg;
		const float Take = FMath::Min(Stored, Remaining);
		if (Take <= 0.f)
		{
			continue;
		}
		Record.StateScalar = (Stored - Take) / Record.Def->FuelCapacityKg;
		MarkRecordDirty(Record);
		Remaining -= Take;
	}
	return Remaining <= KINDA_SMALL_NUMBER;
}

float AVehicleConstruct::GetAscentTwr() const
{
	if (!PhysicsRoot)
	{
		return 0.f;
	}
	const float MassKg = FMath::Max(PhysicsRoot->GetMass(), 1.f);
	const float G = FMath::Abs(GetWorld() ? GetWorld()->GetGravityZ() / ExoneerUnits::CmPerM : 9.81f); // UU/s^2 -> m/s^2

	// The number that answers "can this thing climb, right now, as it sits".
	// Three things it must not lie about, and it used to lie about all three:
	//
	//   - only WORLD-VERTICAL thrust fights weight. Projecting on the craft's
	//     own up axis reported the lift a level craft would have, so a rover
	//     banked 40 degrees read a TWR it did not have;
	//   - the collective commands each unit in proportion to its share of the
	//     craft's up axis, so a canted unit is credited twice over - once for
	//     what the valve asks of it and once for where it points;
	//   - a thruster delivers MaxThrust * throttle * PowerSupplyFraction, and
	//     the pilot's throttle stops at the reserved ceiling. A brownout at
	//     half supply halves the craft's TWR, which is exactly the moment the
	//     pilot needs to be told;
	//   - and the direction is the JET, not the aim. A canted nozzle costs
	//     cos^2 of its lift, and that cost is the price of the craft's yaw
	//     authority. Reading it off the aim axis credited the rover with lift
	//     the cant had already spent.
	const FVector UpAxis = GetActorQuat().GetAxisZ();
	const float SupplyFraction = FMath::Clamp(PowerSupplyFraction, 0.f, 1.f);
	float Ceiling = 1.f;
	float LiftN = 0.f;
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (Record.Phase != EConstructionPhase::Complete || !Record.Def || Record.Def->MaxThrust <= 0.f)
		{
			continue;
		}
		Ceiling = ExoneerThrust::PilotThrottleCeiling(Record.Def->LiftControlReserveFraction);
		const FVector ThrustDirection = GetBlockWorldTransform(Record).TransformVectorNoScale(
			ExoneerThruster::LocalThrustDirection(Record.Def->NozzleCantDeg)).GetSafeNormal();
		const float AlongLift = static_cast<float>(FMath::Max(0.0, FVector::DotProduct(ThrustDirection, UpAxis)));
		const float AlongWorldUp = static_cast<float>(FMath::Max(0.0, FVector::DotProduct(ThrustDirection, FVector::UpVector)));
		LiftN += Record.Def->MaxThrust * SupplyFraction * AlongLift * AlongWorldUp;
	}
	const float WeightN = MassKg * G;
	return WeightN > KINDA_SMALL_NUMBER ? Ceiling * LiftN / WeightN : 0.f;
}

void AVehicleConstruct::ApplyWeatherWear(float StormIntensity, float DtSeconds)
{
	if (!HasAuthority() || StormIntensity <= 0.f || DtSeconds <= 0.f)
	{
		return;
	}
	for (FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (Record.Phase != EConstructionPhase::Complete || !Record.Def)
		{
			continue;
		}
		if (!Record.Def->ModuleClass || !Record.Def->ModuleClass->IsChildOf(USolarModule::StaticClass()))
		{
			continue;
		}
		Record.Condition.SurfaceOpacity01 = FMath::Clamp(
			Record.Condition.SurfaceOpacity01 + ExoneerMaintenance::DustOpacityDelta(StormIntensity, DtSeconds),
			0.f, 1.f);
		MarkRecordDirty(Record);
	}
}

float AVehicleConstruct::GetMinTreadDepthMm() const
{
	float MinTread = -1.f;
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (Record.Phase == EConstructionPhase::Complete && Record.Condition.HasTire())
		{
			MinTread = (MinTread < 0.f) ? Record.Condition.TreadDepthMm
				: FMath::Min(MinTread, Record.Condition.TreadDepthMm);
		}
	}
	return MinTread;
}

float AVehicleConstruct::GetMaxWindingTempC() const
{
	// Worst case across the wheels, like MinTreadDepthMm: one overheating hub
	// is what the driver needs to see, not an average that hides it. Record
	// driven, so the HUD reads the same number on a client.
	float MaxTemp = NoWindingReadingC;
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (Record.Phase == EConstructionPhase::Complete && Record.Def && Record.Def->bIsWheel)
		{
			MaxTemp = FMath::Max(MaxTemp, Record.Condition.WindingTempC);
		}
	}
	return MaxTemp;
}

bool AVehicleConstruct::HasThermalCutout() const
{
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (Record.Phase == EConstructionPhase::Complete && Record.Def && Record.Def->bIsWheel
			&& Record.Condition.bThermalCutout)
		{
			return true;
		}
	}
	return false;
}

bool AVehicleConstruct::IsAnyWindingDerating() const
{
	// The onset temperature is authored per wheel spec, so the comparison
	// belongs here and not in the HUD.
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (Record.Phase == EConstructionPhase::Complete && Record.Def && Record.Def->bIsWheel
			&& Record.Condition.WindingTempC > Record.Def->WheelSpec.DerateOnsetTempC)
		{
			return true;
		}
	}
	return false;
}

float AVehicleConstruct::ApplyExoneerDamage_Implementation(float Amount, EExoneerDamageType Type, AActor* DamageInstigator)
{
	if (!HasAuthority() || Amount <= 0.f || BlockList.Blocks.Num() == 0)
	{
		return 0.f;
	}

	// No hit point on this interface; use the instigator's position and fall
	// back to the nearest block center so the damage always lands somewhere.
	FVector Point = DamageInstigator ? DamageInstigator->GetActorLocation() : GetActorLocation();
	if (FindBlockAtWorldPoint(Point) == INDEX_NONE)
	{
		double BestDistSq = TNumericLimits<double>::Max();
		for (const FVehicleBlockRecord& Record : BlockList.Blocks)
		{
			const FVector Center = GetBlockWorldTransform(Record).GetLocation();
			const double DistSq = FVector::DistSquared(Center, Point);
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				Point = Center;
			}
		}
	}
	return ApplyDamageToBlockAt(Point, Amount, Type, DamageInstigator);
}

// --- IConstructible ---

EConstructionPhase AVehicleConstruct::GetConstructionPhaseAt_Implementation(const FVector& WorldPoint) const
{
	const FVehicleBlockRecord* Record = FindRecord(FindBlockAtWorldPoint(WorldPoint));
	return Record ? Record->Phase : EConstructionPhase::Complete;
}

namespace
{
	/** 0..1 of the authored weld work ONE record has taken, across all its stages. */
	float BlockConstructionProgress01(const FVehicleBlockRecord& Record)
	{
		if (!Record.Def || Record.Phase == EConstructionPhase::Complete)
		{
			return 1.f;
		}
		const TArray<FConstructionCost>& Stages = Record.Def->Stages;
		if (Stages.Num() == 0)
		{
			return 0.f;
		}

		float TotalWork = 0.f;
		float DoneWork = 0.f;
		for (int32 i = 0; i < Stages.Num(); ++i)
		{
			const float Work = FMath::Max(Stages[i].WeldWork, KINDA_SMALL_NUMBER);
			TotalWork += Work;
			if (i < Record.StageIndex)
			{
				DoneWork += Work;
			}
			else if (i == Record.StageIndex)
			{
				DoneWork += FMath::Clamp(Record.StageProgress01, 0.f, 1.f) * Work;
			}
		}
		return TotalWork > 0.f ? DoneWork / TotalWork : 0.f;
	}
}

float AVehicleConstruct::GetConstructionProgressAt_Implementation(const FVector& WorldPoint) const
{
	// Point-addressed, so it inherits the "no record reads as Complete"
	// ambiguity documented on IConstructible. Never route a weld READOUT
	// through here - see GetConstructionProgressForTarget below.
	const FVehicleBlockRecord* Record = FindRecord(FindBlockAtWorldPoint(WorldPoint));
	return Record ? BlockConstructionProgress01(*Record) : 1.f;
}

float AVehicleConstruct::GetConstructionProgressForTarget_Implementation(int32 TargetId) const
{
	// Identity-addressed: an id that names no record answers "unknown", never
	// 1.0. This is what the weld readout reads, so a sweep that grazes the
	// finished neighbour can no longer make the ghost read 100 percent.
	const FVehicleBlockRecord* Record = FindRecord(TargetId);
	return Record ? BlockConstructionProgress01(*Record) : ExoneerConstruction::UnknownProgress;
}

int32 AVehicleConstruct::GetConstructionTargetIdAt_Implementation(const FVector& WorldPoint) const
{
	return FindBlockAtWorldPoint(WorldPoint);
}

float AVehicleConstruct::InvestConstruction_Implementation(AActor* Builder, UInventoryComponent* SourceInventory, const FVector& WorldPoint, float WeldPoints, int32& OutTargetId)
{
	OutTargetId = ExoneerConstruction::NoTargetId;
	if (!HasAuthority() || WeldPoints <= 0.f)
	{
		return 0.f;
	}
	FVehicleBlockRecord* Record = FindMutableRecord(FindBlockAtWorldPoint(WorldPoint));

	// On a moving construct the aim often grazes the finished neighbor of the
	// intended ghost; prefer the nearest unfinished block within two cells
	// before concluding there is nothing to weld here.
	if (Record && Record->Phase == EConstructionPhase::Complete)
	{
		FVehicleBlockRecord* NearestUnfinished = nullptr;
		double BestDistSq = FMath::Square(CellSize * 2.0);
		for (FVehicleBlockRecord& Candidate : BlockList.Blocks)
		{
			if (Candidate.Phase == EConstructionPhase::Complete || !Candidate.Def)
			{
				continue;
			}
			const double DistSq = FVector::DistSquared(GetBlockWorldTransform(Candidate).GetLocation(), WorldPoint);
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				NearestUnfinished = &Candidate;
			}
		}
		if (NearestUnfinished)
		{
			Record = NearestUnfinished;
		}
	}

	if (!Record || !Record->Def || Record->Phase == EConstructionPhase::Complete)
	{
		return 0.f;
	}

	// From here the weld can only land on THIS record, whichever block the aim
	// point named. Report it, so the readout follows the block being welded
	// rather than whatever the 14 cm sweep happened to touch.
	OutTargetId = Record->BlockInstanceId;

	const TArray<FConstructionCost>& Stages = Record->Def->Stages;
	if (Stages.Num() == 0)
	{
		// No investment plan: welding once completes the block.
		Record->Phase = EConstructionPhase::Complete;
		Record->StageProgress01 = 1.f;
		Record->Health = Record->Def->MaxHealth;
		MarkRecordDirty(*Record);
		MarkVisualsDirty();
		return WeldPoints;
	}

	float Remaining = WeldPoints;
	float Applied = 0.f;
	bool bChanged = false;

	while (Remaining > KINDA_SMALL_NUMBER && Record->Phase != EConstructionPhase::Complete)
	{
		Record->StageIndex = FMath::Clamp(Record->StageIndex, 0, Stages.Num() - 1);
		const FConstructionCost& Stage = Stages[Record->StageIndex];
		const float StageWork = FMath::Max(Stage.WeldWork, KINDA_SMALL_NUMBER);
		const float StartProgress = FMath::Clamp(Record->StageProgress01, 0.f, 1.f);
		const float TargetProgress = FMath::Min(1.f, StartProgress + Remaining / StageWork);

		// Materials are consumed proportionally across the stage: at progress P
		// each material owes ceil(Count * P) units. Missing units cap progress
		// at the threshold they would have crossed (same rule as
		// UConstructionComponent, spec section 7). Creative mode waives both
		// the availability cap and the consumption below.
		const bool bCreativeWeld = ExoneerCreative::IsEnabled();
		float ReachableProgress = TargetProgress;
		for (const FInventoryEntry& Material : Stage.Materials)
		{
			if (bCreativeWeld)
			{
				break;
			}
			UItemDefinitionDataAsset* Item = Material.Item.LoadSynchronous();
			if (!Item || Material.Count <= 0)
			{
				continue;
			}
			const int32 ConsumedSoFar = UnitsOwedAtProgress(Material.Count, StartProgress);
			const int32 OwedAtTarget = UnitsOwedAtProgress(Material.Count, TargetProgress);
			const int32 Needed = OwedAtTarget - ConsumedSoFar;
			const int32 Available = SourceInventory ? SourceInventory->GetItemCount(Item) : 0;
			if (Needed > Available)
			{
				ReachableProgress = FMath::Min(ReachableProgress, float(ConsumedSoFar + Available) / float(Material.Count));
			}
		}
		ReachableProgress = FMath::Clamp(ReachableProgress, StartProgress, TargetProgress);
		if (ReachableProgress <= StartProgress + KINDA_SMALL_NUMBER)
		{
			break; // Starved of materials at the next threshold.
		}

		// Pull the units whose thresholds this advance crosses.
		for (const FInventoryEntry& Material : Stage.Materials)
		{
			if (bCreativeWeld)
			{
				break;
			}
			UItemDefinitionDataAsset* Item = Material.Item.LoadSynchronous();
			if (!Item || Material.Count <= 0 || !SourceInventory)
			{
				continue;
			}
			const int32 ToConsume = UnitsOwedAtProgress(Material.Count, ReachableProgress) - UnitsOwedAtProgress(Material.Count, StartProgress);
			if (ToConsume > 0)
			{
				SourceInventory->RemoveItem(Item, ToConsume);
			}
		}

		const float Work = (ReachableProgress - StartProgress) * StageWork;
		Applied += Work;
		Remaining -= Work;
		Record->StageProgress01 = ReachableProgress;
		bChanged = true;

		if (Record->Phase == EConstructionPhase::Ghost)
		{
			Record->Phase = EConstructionPhase::UnderConstruction;
			MarkVisualsDirty();
		}

		if (Record->StageProgress01 >= 1.f - KINDA_SMALL_NUMBER)
		{
			if (Record->StageIndex >= Stages.Num() - 1)
			{
				Record->StageIndex = Stages.Num() - 1;
				Record->StageProgress01 = 1.f;
				Record->Phase = EConstructionPhase::Complete;
				Record->Health = Record->Def->MaxHealth;
				if (Record->Def->FuelCapacityKg > 0.f)
				{
					Record->StateScalar = 1.f;
				}
				Record->bDeconstructPenalty = 0; // Welded back whole: penalty resets.
				MarkVisualsDirty(); // Box welds in; module sync picks it up next tick.
			}
			else
			{
				++Record->StageIndex;
				Record->StageProgress01 = 0.f;
			}
		}
	}

	if (bChanged)
	{
		MarkRecordDirty(*Record);
	}
	if (Applied <= 0.f)
	{
		// Starved at the first material threshold: no work went anywhere, so
		// name no target and let the caller show no progress.
		OutTargetId = ExoneerConstruction::NoTargetId;
	}
	return Applied;
}

float AVehicleConstruct::DeconstructAt_Implementation(AActor* Builder, UInventoryComponent* RefundInventory, const FVector& WorldPoint, float WreckPoints)
{
	if (!HasAuthority() || WreckPoints <= 0.f)
	{
		return 0.f;
	}
	const int32 BlockId = FindBlockAtWorldPoint(WorldPoint);
	FVehicleBlockRecord* Record = FindMutableRecord(BlockId);
	if (!Record || !Record->Def)
	{
		return 0.f;
	}

	const TArray<FConstructionCost>& Stages = Record->Def->Stages;
	const bool bPristineGhost = Record->Phase == EConstructionPhase::Ghost
		&& Record->StageIndex == 0
		&& Record->StageProgress01 <= 0.f;
	if (Stages.Num() == 0 || bPristineGhost)
	{
		// Nothing was invested; just take the block off the grid.
		RemoveBlock(BlockId);
		return WreckPoints;
	}

	// A block that was ever Complete refunds at half for the WHOLE reversal;
	// the sticky flag survives the phase downgrade below so later wreck ticks
	// cannot escape the penalty. Welding back to Complete clears it.
	if (Record->Phase == EConstructionPhase::Complete)
	{
		Record->bDeconstructPenalty = 1;
		Record->StageIndex = Stages.Num() - 1;
		Record->StageProgress01 = 1.f;
		Record->Phase = EConstructionPhase::UnderConstruction;
		MarkVisualsDirty(); // De-weld the box; module sync drops the module.
	}
	const float RefundMult = Record->bDeconstructPenalty ? 0.5f : 1.f;

	float Remaining = WreckPoints;
	float Applied = 0.f;

	while (Remaining > KINDA_SMALL_NUMBER)
	{
		Record->StageIndex = FMath::Clamp(Record->StageIndex, 0, Stages.Num() - 1);
		const FConstructionCost& Stage = Stages[Record->StageIndex];
		const float StageWork = FMath::Max(Stage.WeldWork, KINDA_SMALL_NUMBER);
		const float StartProgress = FMath::Clamp(Record->StageProgress01, 0.f, 1.f);
		const float NewProgress = FMath::Max(0.f, StartProgress - Remaining / StageWork);

		// Refund the material units whose thresholds we cross on the way down.
		for (const FInventoryEntry& Material : Stage.Materials)
		{
			UItemDefinitionDataAsset* Item = Material.Item.LoadSynchronous();
			if (!Item || Material.Count <= 0)
			{
				continue;
			}
			const int32 UnitsCrossed = UnitsOwedAtProgress(Material.Count, StartProgress) - UnitsOwedAtProgress(Material.Count, NewProgress);
			const int32 RefundUnits = FMath::RoundToInt(UnitsCrossed * RefundMult);
			// Creative welds invested nothing, so creative deconstruction
			// refunds nothing - otherwise this is an item duplicator.
			if (RefundUnits > 0 && RefundInventory && !ExoneerCreative::IsEnabled())
			{
				RefundInventory->AddItem(Item, RefundUnits); // Overflow is lost.
			}
		}

		const float Work = (StartProgress - NewProgress) * StageWork;
		Applied += Work;
		Remaining -= Work;
		Record->StageProgress01 = NewProgress;

		if (NewProgress <= 0.f)
		{
			if (Record->StageIndex > 0)
			{
				--Record->StageIndex;
				Record->StageProgress01 = 1.f;
			}
			else
			{
				// Fully reversed to an empty ghost: remove it from the grid.
				Record->Phase = EConstructionPhase::Ghost;
				RemoveBlock(BlockId);
				return Applied;
			}
		}
	}

	MarkRecordDirty(*Record);
	return Applied;
}

// --- Piloting ---

void AVehicleConstruct::SetPilotInput(const FPilotInput& Input)
{
	if (!HasAuthority())
	{
		return;
	}
	PilotInput = Input;
	PilotInput.Sanitize();
	LastPilotInputServerTime = GetWorld()->GetTimeSeconds();

	// Mode toggle rides a rolling 2-bit counter so presses between 20 Hz sends
	// latch losslessly. The first packet after a seating only adopts the
	// client's current counter (its value is arbitrary at that point).
	if (bModeToggleSyncPending)
	{
		bModeToggleSyncPending = false;
		LastProcessedModeToggle = PilotInput.ModeToggleCount;
	}
	else
	{
		const uint8 Presses = (PilotInput.ModeToggleCount - LastProcessedModeToggle) & 0x3;
		LastProcessedModeToggle = PilotInput.ModeToggleCount;
		if (Presses % 2 == 1)
		{
			// Never strand the pilot in a mode the hardware cannot use: a
			// thruster-less rover has nothing to actuate in Flight (and the
			// mouse stops being a camera), a wheel-less flyer nothing in
			// Ground. The toggle only lands on a mode with matching blocks.
			const bool bWantGround = ControlMode == EPilotControlMode::Flight;
			if (bWantGround ? HasCompleteWheel() : HasCompleteThruster())
			{
				ControlMode = bWantGround ? EPilotControlMode::Ground : EPilotControlMode::Flight;
			}
		}
	}
}

void AVehicleConstruct::ApplyPilotInput_Implementation(const FPilotInput& Input)
{
	SetPilotInput(Input);
}

bool AVehicleConstruct::EnterPilot_Implementation(APawn* Pilot, int32 StationId)
{
	if (!HasAuthority() || !Pilot || PilotPawn)
	{
		return false;
	}
	const FVehicleBlockRecord* Record = FindRecord(StationId);
	if (!Record || !IsCockpitRecord(*Record))
	{
		return false;
	}

	// A pawn already seated elsewhere must exit that construct first; seating
	// it here too would leave two constructs referencing one pilot.
	if (const APlayerSurvivalCharacter* Character = Cast<APlayerSurvivalCharacter>(Pilot))
	{
		if (Character->PilotedConstruct && Character->PilotedConstruct != this)
		{
			return false;
		}
	}

	PilotPawn = Pilot;
	ActiveCockpitId = StationId;

	// Fresh seat: adopt the pilot's rolling toggle counter on the first packet
	// instead of interpreting its arbitrary current value as presses.
	bModeToggleSyncPending = true;
	PilotInput = FPilotInput();
	LastPilotInputServerTime = -1.0;

	// A fresh seat starts from the craft's real attitude and a closed lift
	// valve, never from whatever the last pilot left behind.
	bAttitudeReferenceValid = false;
	LiftCollective = 0.f;
	bLiftGovernorActive = false;
	bLiftInverted = false;
	UntrimmedStandingMomentNm = 0.f;
	StandingSideForceN = 0.f;
	// The seat itself is not proof of a live pilot; the first packet is.
	bPilotInputStale = true;

	// Board in a mode the craft can actually use. Hybrids keep whatever the
	// pilot last chose; single-capability craft snap to their only option.
	const bool bCanDrive = HasCompleteWheel();
	const bool bCanFly = HasCompleteThruster();
	if (bCanDrive && !bCanFly)
	{
		ControlMode = EPilotControlMode::Ground;
	}
	else if (bCanFly && !bCanDrive)
	{
		ControlMode = EPilotControlMode::Flight;
	}

	// Seat the pawn: attach to the root, then snap to the cockpit block. The
	// character's own movement simulation and capsule collision must sleep
	// while seated, or CMC gravity/ServerMoves fight the seat transform and
	// the capsule kicks the vehicle rigid body around.
	Pilot->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
	if (USceneComponent* PilotRoot = Pilot->GetRootComponent())
	{
		PilotRoot->SetRelativeTransform(GetBlockLocalTransform(*Record));
	}
	if (ACharacter* AsCharacter = Cast<ACharacter>(Pilot))
	{
		if (UCharacterMovementComponent* Movement = AsCharacter->GetCharacterMovement())
		{
			Movement->StopMovementImmediately();
			Movement->SetMovementMode(MOVE_None);   // Replicates via ReplicatedMovementMode.
		}
		if (UCapsuleComponent* Capsule = AsCharacter->GetCapsuleComponent())
		{
			Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
	}

	if (APlayerSurvivalCharacter* Character = Cast<APlayerSurvivalCharacter>(Pilot))
	{
		Character->SetPilotedConstruct(this);
	}
	return true;
}

void AVehicleConstruct::ExitPilot_Implementation(APawn* Pilot)
{
	if (!HasAuthority() || !PilotPawn)
	{
		return;
	}
	if (Pilot && Pilot != PilotPawn)
	{
		return;
	}

	APawn* Leaving = PilotPawn;
	PilotPawn = nullptr;
	ActiveCockpitId = INDEX_NONE;
	PilotInput = FPilotInput();
	LastPilotInputServerTime = -1.0;
	// Symmetry with EnterPilot, and it stops up to half a second of unpiloted
	// thrust aimed at the pawn this function is about to step out beside the
	// hull. The unpiloted branch was already driving it here, just slower.
	LiftCollective = 0.f;
	bAttitudeReferenceValid = false;
	bLiftGovernorActive = false;
	bLiftInverted = false;
	UntrimmedStandingMomentNm = 0.f;
	StandingSideForceN = 0.f;
	bPilotInputStale = true;

	if (IsValid(Leaving))
	{
		Leaving->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);

		// Restore what EnterPilot suspended, and step the pawn clear of the
		// hull so it does not un-collide inside a block box.
		if (ACharacter* AsCharacter = Cast<ACharacter>(Leaving))
		{
			if (UCapsuleComponent* Capsule = AsCharacter->GetCapsuleComponent())
			{
				Capsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			}
			if (UCharacterMovementComponent* Movement = AsCharacter->GetCharacterMovement())
			{
				Movement->SetMovementMode(MOVE_Falling);
			}
			// Step clear along WORLD up, not the construct's Z: a rover on its
			// roof has its local Z pointing into the ground, which teleported
			// the engineer INTO the floor. Also clear sideways from the hull so
			// a flipped or half-buried vehicle does not eject the pilot inside
			// its own blocks.
			const float ClearHeight = AsCharacter->GetCapsuleComponent()
				? AsCharacter->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + CellSize
				: 100.f;
			FVector LateralOut = AsCharacter->GetActorLocation() - GetActorLocation();
			LateralOut.Z = 0.f;
			if (!LateralOut.Normalize(1e-3f))
			{
				LateralOut = GetActorForwardVector().GetSafeNormal2D();
			}
			AsCharacter->SetActorLocation(
				AsCharacter->GetActorLocation() + FVector::UpVector * ClearHeight + LateralOut * CellSize * 2.f,
				false, nullptr, ETeleportType::TeleportPhysics);
		}
		if (APlayerSurvivalCharacter* Character = Cast<APlayerSurvivalCharacter>(Leaving))
		{
			Character->SetPilotedConstruct(nullptr);
		}
	}
}

void AVehicleConstruct::OnRep_Pilot()
{
	// Cosmetic hook only; seat FX/camera handling belongs in Blueprints.
}

// --- IInteractable ---

bool AVehicleConstruct::OnInteract_Implementation(AActor* Interactor)
{
	APawn* Pawn = Cast<APawn>(Interactor);
	if (!HasAuthority() || !Pawn)
	{
		return false;
	}

	if (Pawn == PilotPawn)
	{
		IPilotable::Execute_ExitPilot(this, Pawn);
		return true;
	}

	// Seat the pawn at the nearest finished cockpit.
	int32 BestId = INDEX_NONE;
	double BestDistSq = TNumericLimits<double>::Max();
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (!IsCockpitRecord(Record))
		{
			continue;
		}
		const double DistSq = FVector::DistSquared(GetBlockWorldTransform(Record).GetLocation(), Pawn->GetActorLocation());
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			BestId = Record.BlockInstanceId;
		}
	}
	if (BestId == INDEX_NONE)
	{
		return false;
	}
	return IPilotable::Execute_EnterPilot(this, Pawn, BestId);
}

FText AVehicleConstruct::GetInteractionPrompt_Implementation() const
{
	return NSLOCTEXT("Exoneer", "PilotConstruct", "Pilot");
}

FGameplayTagContainer AVehicleConstruct::GetInteractionTags_Implementation() const
{
	FGameplayTagContainer InteractionTags;
	InteractionTags.AddTag(ExoneerTags::Interaction_Pilot);
	return InteractionTags;
}

// --- Tick ---

void AVehicleConstruct::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bVisualsDirty)
	{
		bVisualsDirty = false;
		RebuildDerivedState();
	}

	if (HasAuthority())
	{
		SyncModulesToRecords();

		// Hold the last pilot packet until the next one arrives; past the
		// timeout (pilot lag/disconnect) zero the axes and engage the parking
		// brake. This replaces the old per-tick decay, which sagged intents
		// ~40 percent between 20 Hz sends and made held throttle mushy.
		const bool bInputTimedOut = LastPilotInputServerTime < 0.0
			|| GetWorld()->GetTimeSeconds() - LastPilotInputServerTime > PilotInputTimeoutSeconds;
		if (bInputTimedOut)
		{
			PilotInput.ZeroAxes();
		}
		bParkingBrakeEngaged = bInputTimedOut || !PilotPawn
			|| (PilotInput.HeldFlags & EPilotHeldFlags::Handbrake) != 0;
		// The lift governor needs a live pilot, not just a quiet packet: a
		// released key and a dead connection look identical in the axes.
		bPilotInputStale = bInputTimedOut || !PilotPawn;

		// Physics-thread telemetry lands before the ledger reads motor draws.
		DrainWheelTelemetry();

		// Landing damage: peak downward speed while airborne, then slam.
		{
			int32 Contact = 0;
			for (const FVehicleWheelStateItem& Item : WheelStates.Items)
			{
				if (Item.CompressionQ > 0)
				{
					++Contact;
				}
			}
			// Two-edge debounce on the ground/air decision. The state flips
			// only after the OTHER answer has held for the authored release
			// time, in both directions, so one wheel tap on rough ground
			// cannot flip it and neither can one frame of lost contact. The
			// predicate is external SUPPORT rather than tyre compression, so a
			// wheel-less hull resting on the ground counts too.
			{
				const bool bSupported = IsSupportedByGround();
				float ReleaseSeconds = 0.25f;
				for (const FVehicleBlockRecord& Record : BlockList.Blocks)
				{
					if (Record.Phase == EConstructionPhase::Complete && Record.Def
						&& Record.Def->MaxGyroTorqueNm > 0.f)
					{
						ReleaseSeconds = FMath::Max(0.f, Record.Def->AttitudeGroundReleaseSeconds);
						break;
					}
				}
				if (bSupported == bGroundContactLatched)
				{
					GroundStateTimerSeconds = 0.f;
				}
				else
				{
					GroundStateTimerSeconds += DeltaSeconds;
					if (GroundStateTimerSeconds >= ReleaseSeconds)
					{
						bGroundContactLatched = bSupported;
						GroundStateTimerSeconds = 0.f;
					}
				}
			}

			const bool bAirborne = PhysicsRoot && PhysicsRoot->IsSimulatingPhysics()
				&& (HasCompleteWheel() ? Contact == 0 : GetVelocity().Z < -50.f);
			if (bAirborne)
			{
				AirborneDownSpeedUU = FMath::Max(AirborneDownSpeedUU, -GetVelocity().Z);
			}
			else if (AirborneDownSpeedUU > 800.f) // 8 m/s
			{
				const float SpeedMS = AirborneDownSpeedUU / ExoneerUnits::CmPerM;
				LastLandingSpeedMS = SpeedMS;
				const float Excess = SpeedMS - 8.f;
				const float Damage = Excess * 25.f; // ~25 HP per m/s over 8
				ApplyExoneerDamage_Implementation(Damage, EExoneerDamageType::Impact, this);
				AirborneDownSpeedUU = 0.f;
			}
			else
			{
				AirborneDownSpeedUU = 0.f;
			}

			if (PhysicsRoot)
			{
				// Restore the ground values when flight ends, too. This path
				// only ever SET the flying values, so one hop left a rover
				// permanently over-damped until the next RebuildDerivedState.
				// A PILOT check, not just a mode check: an abandoned rover
				// left in Flight mode was using flight damping for ever.
				const bool bFlying = ControlMode == EPilotControlMode::Flight
					&& PilotPawn != nullptr && HasCompleteThruster();
				const bool bWheeled = HasCompleteWheel();
				PhysicsRoot->SetLinearDamping(bFlying ? LinearDampingFlying
					: (bWheeled ? LinearDampingWheeled : LinearDampingNoWheels));
				PhysicsRoot->SetAngularDamping(bFlying ? AngularDampingFlying
					: (bWheeled ? AngularDampingWheeled : AngularDampingNoWheels));
			}
		}

		ServerTickPowerLedger(DeltaSeconds);
		ServerRouteThrust(DeltaSeconds);
		ServerRouteDrive(DeltaSeconds);
		ServerTickModules(DeltaSeconds);
		// Wheel modules refreshed their ground caches in their tick; marshal
		// this frame's input before the solver advances (we run TG_PrePhysics).
		MarshalWheelPhysics();
	}

	// Both sides: pose the animated wheel meshes from the replicated state.
	UpdateWheelVisuals(DeltaSeconds);
}

void AVehicleConstruct::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (WheelSimCallback)
	{
		// The solver owns the object after unregister; never delete or touch
		// it again. Re-fetch the solver - caching it across frames is unsafe.
		if (UWorld* World = GetWorld())
		{
			if (FPhysScene* Scene = World->GetPhysicsScene())
			{
				if (Chaos::FPhysicsSolver* Solver = Scene->GetSolver())
				{
					Solver->UnregisterAndFreeSimCallbackObject_External(WheelSimCallback);
				}
			}
		}
		WheelSimCallback = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

void AVehicleConstruct::DrainWheelTelemetry()
{
	if (!WheelSimCallback)
	{
		return;
	}
	while (Chaos::TSimCallbackOutputHandle<FExoneerWheelSimOutput> Output = WheelSimCallback->PopOutputData_External())
	{
		for (const ExoneerWheelSim::FWheelSimTelemetry& Telemetry : Output->Wheels)
		{
			if (UWheelModule* Wheel = Cast<UWheelModule>(Modules.FindRef(Telemetry.BlockInstanceId)))
			{
				Wheel->ConsumeTelemetry(Telemetry);
			}
		}
	}
}

void AVehicleConstruct::MarshalWheelPhysics()
{
	// Collect the live wheel modules with valid ground caches this frame.
	TArray<UWheelModule*, TInlineAllocator<12>> WheelModules;
	for (const TPair<int32, TObjectPtr<UVehicleModule>>& Pair : Modules)
	{
		if (UWheelModule* Wheel = Cast<UWheelModule>(Pair.Value.Get()))
		{
			WheelModules.Add(Wheel);
		}
	}
	if (WheelModules.Num() == 0 || !PhysicsRoot || !PhysicsRoot->IsSimulatingPhysics())
	{
		return;
	}

	if (!WheelSimCallback)
	{
		UWorld* World = GetWorld();
		FPhysScene* Scene = World ? World->GetPhysicsScene() : nullptr;
		Chaos::FPhysicsSolver* Solver = Scene ? Scene->GetSolver() : nullptr;
		if (!Solver)
		{
			return;
		}
		WheelSimCallback = Solver->CreateAndRegisterSimCallbackObject_External<FExoneerWheelSimCallback>();
	}

	FExoneerWheelSimInput* Input = WheelSimCallback->GetProducerInputData_External();
	// The producer object is sticky until a physics step consumes it - Reset
	// gives last-write-wins instead of accumulating stale wheel entries.
	Input->Reset();

	FBodyInstance* Body = PhysicsRoot->GetBodyInstance();
	Input->Proxy = Body ? Body->GetPhysicsActor() : nullptr;
	if (!Input->Proxy)
	{
		return;
	}

	Input->Wheels.Reserve(WheelModules.Num());
	bool bWantsWake = false;
	for (UWheelModule* Wheel : WheelModules)
	{
		ExoneerWheelSim::FWheelSimInputItem Item;
		if (Wheel->BuildSimInput(Item))
		{
			bWantsWake |= FMath::Abs(Item.Command.Throttle) > 0.05f || Item.Command.Brake > 0.05f;
			Input->Wheels.Add(MoveTemp(Item));
		}
	}

	// Wheel forces are applied with bInvalidate=false so a parked rover can
	// sleep; drive input must therefore wake the body explicitly.
	if (bWantsWake && !PhysicsRoot->RigidBodyIsAwake())
	{
		PhysicsRoot->WakeAllRigidBodies();
	}
}

void AVehicleConstruct::QueueWheelStateRestore(int32 BlockInstanceId, float TirePressureKPa, float SteerTrimDeg)
{
	FWheelSavedState& State = PendingWheelRestore.FindOrAdd(BlockInstanceId);
	State.TirePressureKPa = TirePressureKPa;
	State.SteerTrimDeg = SteerTrimDeg;
}

bool AVehicleConstruct::TakeSavedWheelState(int32 BlockInstanceId, FWheelSavedState& OutState)
{
	if (const FWheelSavedState* Found = PendingWheelRestore.Find(BlockInstanceId))
	{
		OutState = *Found;
		PendingWheelRestore.Remove(BlockInstanceId);
		return true;
	}
	return false;
}

bool AVehicleConstruct::GetWheelPersistentState(int32 BlockInstanceId, float& OutTirePressureKPa, float& OutSteerTrimDeg) const
{
	if (const UWheelModule* Wheel = Cast<UWheelModule>(Modules.FindRef(BlockInstanceId)))
	{
		OutTirePressureKPa = Wheel->GetTirePressureKPa();
		OutSteerTrimDeg = Wheel->GetSteerTrimDeg();
		return true;
	}
	// Not Complete yet (ghost/under construction): keep any still-queued
	// restore values so an unfinished save-load round trip stays lossless.
	if (const FWheelSavedState* Pending = PendingWheelRestore.Find(BlockInstanceId))
	{
		OutTirePressureKPa = Pending->TirePressureKPa;
		OutSteerTrimDeg = Pending->SteerTrimDeg;
		return true;
	}
	return false;
}

// --- Server internals ---

void AVehicleConstruct::SyncModulesToRecords()
{
	if (!HasAuthority())
	{
		return;
	}

	TSet<int32> LiveIds;
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (Record.Phase != EConstructionPhase::Complete || !Record.Def || !Record.Def->ModuleClass)
		{
			continue;
		}
		LiveIds.Add(Record.BlockInstanceId);
		if (!Modules.Contains(Record.BlockInstanceId))
		{
			UVehicleModule* Module = NewObject<UVehicleModule>(this, Record.Def->ModuleClass);
			Module->Initialize(this, Record.BlockInstanceId);
			Modules.Add(Record.BlockInstanceId, Module);
		}
	}

	for (auto It = Modules.CreateIterator(); It; ++It)
	{
		if (!LiveIds.Contains(It->Key))
		{
			if (It->Value)
			{
				It->Value->Shutdown();
			}
			It.RemoveCurrent();
		}
	}
}

void AVehicleConstruct::ServerTickPowerLedger(float DeltaSeconds)
{
	if (DeltaSeconds <= 0.f)
	{
		return;
	}

	// Ambient the packs age in, from the same day/night curve the windings
	// use; 20 C when the map has no environment manager.
	float AmbientC = 20.f;
	if (const APlanetEnvironmentManager* Environment = GetEnvironmentManager())
	{
		AmbientC = Environment->GetCurrentAmbientTemperatureC();
	}

	// A pack holds only what its cells have left. StateScalar is a fraction of
	// THIS, not of the rating, so every ledger read below goes through it.
	const auto EffectiveCapacityJ = [](const FVehicleBlockRecord& Record)
	{
		return ExoneerMaintenance::EffectiveCapacityJ(Record.Def->EnergyStorage, Record.Condition.CapacityFade01);
	};

	// Replicate battery charge in 1% steps to keep the fast array quiet, and
	// age the pack by the joules that just moved through it. Throughput on a
	// multi-battery construct therefore splits by effective capacity, because
	// the ledger already charges and drains proportionally to it.
	const auto SetBatteryCharge = [this, AmbientC, &EffectiveCapacityJ](FVehicleBlockRecord& Record, float NewCharge)
	{
		NewCharge = FMath::Clamp(NewCharge, 0.f, 1.f);
		const float OldCharge = Record.StateScalar;
		Record.StateScalar = NewCharge;

		bool bFadeMoved = false;
		if (!ExoneerMaintenance::IsCapacityTerminal(Record.Condition.CapacityFade01))
		{
			const float ThroughputJ = FMath::Abs(NewCharge - OldCharge) * EffectiveCapacityJ(Record);
			Record.PendingCapacityFade += ExoneerMaintenance::CapacityFadeDelta(
				ThroughputJ, Record.Def->EnergyStorage, AmbientC);
			if (Record.PendingCapacityFade >= ExoneerMaintenance::CapacityFadeDeadband)
			{
				Record.Condition.CapacityFade01 = ExoneerMaintenance::ApplyCapacityFade(
					Record.Condition.CapacityFade01, Record.PendingCapacityFade);
				Record.PendingCapacityFade = 0.f;
				bFadeMoved = true;
			}
		}

		if (bFadeMoved || FMath::FloorToInt(OldCharge * 100.f) != FMath::FloorToInt(NewCharge * 100.f))
		{
			BlockList.MarkItemDirty(Record);
		}
	};
	const auto IsBattery = [](const FVehicleBlockRecord& Record)
	{
		return Record.Phase == EConstructionPhase::Complete && Record.Def && Record.Def->EnergyStorage > 0.f;
	};

	float Production = 0.f;
	float Demand = 0.f;
	for (const TPair<int32, TObjectPtr<UVehicleModule>>& Pair : Modules)
	{
		if (Pair.Value)
		{
			Production += Pair.Value->GetCurrentProduction();
			Demand += Pair.Value->GetCurrentDraw();
		}
	}
	// Module-less complete blocks still count with their idle PowerDelta.
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (Record.Phase != EConstructionPhase::Complete || !Record.Def || Record.Def->ModuleClass)
		{
			continue;
		}
		if (Record.Def->PowerDelta > 0.f)
		{
			Production += Record.Def->PowerDelta;
		}
		else
		{
			Demand += -Record.Def->PowerDelta;
		}
	}

	float SuitChargeW = 0.f;
	APlayerSurvivalCharacter* Pilot = Cast<APlayerSurvivalCharacter>(PilotPawn);
	if (Pilot && Pilot->Survival && Pilot->Survival->SuitPower < Pilot->Survival->SuitPowerCapacityKJ - KINDA_SMALL_NUMBER)
	{
		SuitChargeW = FMath::Max(CockpitSuitChargeW, 0.f);
		Demand += SuitChargeW;
	}

	float NewFraction = 1.f;
	const float Deficit = Demand - Production;
	if (Deficit > KINDA_SMALL_NUMBER)
	{
		// Batteries discharge proportionally to their stored energy.
		float StoredEnergy = 0.f;
		for (const FVehicleBlockRecord& Record : BlockList.Blocks)
		{
			if (IsBattery(Record))
			{
				StoredEnergy += Record.StateScalar * EffectiveCapacityJ(Record);
			}
		}
		const float NeededEnergy = Deficit * DeltaSeconds;
		const float Drawn = FMath::Min(NeededEnergy, StoredEnergy);
		if (Drawn > 0.f && StoredEnergy > 0.f)
		{
			const float DrainFraction = Drawn / StoredEnergy;
			for (FVehicleBlockRecord& Record : BlockList.Blocks)
			{
				if (IsBattery(Record))
				{
					SetBatteryCharge(Record, Record.StateScalar * (1.f - DrainFraction));
				}
			}
		}
		const float Supply = Production + Drawn / DeltaSeconds;
		NewFraction = (Demand <= Supply + KINDA_SMALL_NUMBER) ? 1.f : FMath::Clamp(Supply / Demand, 0.f, 1.f);
	}
	else
	{
		// Charge batteries from the surplus, in record order.
		float SurplusEnergy = -Deficit * DeltaSeconds;
		for (FVehicleBlockRecord& Record : BlockList.Blocks)
		{
			if (SurplusEnergy <= 0.f)
			{
				break;
			}
			if (!IsBattery(Record))
			{
				continue;
			}
			const float Capacity = EffectiveCapacityJ(Record);
			const float Room = (1.f - Record.StateScalar) * Capacity;
			const float Added = FMath::Min(Room, SurplusEnergy);
			if (Added > 0.f && Capacity > 0.f)
			{
				SetBatteryCharge(Record, Record.StateScalar + Added / Capacity);
				SurplusEnergy -= Added;
			}
		}
	}
	PowerSupplyFraction = NewFraction;

	if (SuitChargeW > 0.f && Pilot && Pilot->Survival)
	{
		const float ChargeKJ = SuitChargeW * NewFraction * DeltaSeconds / 1000.f;
		if (ChargeKJ > 0.f)
		{
			Pilot->Survival->AddSuitPower(ChargeKJ);
		}
	}
}

FVector AVehicleConstruct::GetBodyInertiaKgM2() const
{
	if (!PhysicsRoot)
	{
		return FVector::ZeroVector;
	}
	const FBodyInstance* Body = PhysicsRoot->GetBodyInstance();
	if (!Body || !Body->IsValidBodyInstance())
	{
		return FVector::ZeroVector;
	}
	// Chaos keeps the inertia tensor in kg*cm^2; the attitude library is pure SI.
	return Body->GetBodyInertiaTensor() / (ExoneerUnits::CmPerM * ExoneerUnits::CmPerM);
}

void AVehicleConstruct::ServerRouteThrust(float DeltaSeconds)
{
	if (!PhysicsRoot || !PhysicsRoot->IsSimulatingPhysics() || DeltaSeconds <= 0.f)
	{
		return;
	}

	// THRUST is a flight-mode function: on the ground the wheels move the
	// vehicle. ATTITUDE is not gated on mode or on contact - see the rate null
	// below - because the triad rate-nulls whenever a pilot is aboard, which is
	// what the owner had before and did not complain about, and because V must
	// never be a tumble switch at altitude.
	const bool bFlightControl = ControlMode == EPilotControlMode::Flight && PilotPawn != nullptr;

	const FVehicleBlockRecord* Cockpit = FindRecord(ActiveCockpitId);
	FQuat CockpitQuat = GetActorQuat();
	if (Cockpit)
	{
		CockpitQuat = GetActorQuat() * ExoneerVehicleOrientation::GetQuat(Cockpit->Orientation);
	}

	// --- Pilot intent: a horizontal direction, and lift as up / down / hold ---
	// The two are kept apart on purpose. Normalising all three axes together
	// meant forward+lift resolved to 0.707 on each, dropping lift below weight,
	// so asking to fly forward made the craft sink.
	//
	// THE LIFT CONTROL IS THREE-STATE AND NOTHING LATCHES:
	//   lift key held (Move.Z)      -> the valve opens to its reserved ceiling
	//   descend key held            -> the valve closes
	//   neither, and airborne       -> the valve seeks the HOVER setting
	// Every path that ends the pilot's authority - a wheel back on the ground,
	// an input timeout, leaving the seat, switching to Ground - closes the
	// valve, and so does the descend key at any time. The hover state is a
	// governor with a rate term and NO integrator, so it holds the altitude the
	// pilot left it at and cannot run away. The previous latched collective had
	// no key that shut it and no readout of where it had been left, which is
	// what "the thrusters keep firing" was; a craft that can only climb at
	// 0.32 g or fall at 1 g is the opposite failure and just as unflyable.
	FVector HorizontalIntentWorld = FVector::ZeroVector;
	float HorizontalDemand = 0.f;
	if (bFlightControl && Cockpit)
	{
		const FVector Move = PilotInput.Move.BoundToCube(1.f);
		HorizontalDemand = FMath::Clamp(static_cast<float>(FVector2D(Move.X, Move.Y).Size()), 0.f, 1.f);
		if (HorizontalDemand > KINDA_SMALL_NUMBER)
		{
			HorizontalIntentWorld = (CockpitQuat.GetAxisX() * Move.X + CockpitQuat.GetAxisY() * Move.Y).GetSafeNormal();
		}
	}

	// Valve rate, control reserve and governor gain come off the thruster
	// definition: they are properties of the valve hardware, not of the hull.
	float SlewPerSec = 2.f;
	float ReserveFraction = 0.1f;
	float HoverDampingPerMS = 0.1f;
	float DescentRateMS = 2.5f;
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (Record.Phase == EConstructionPhase::Complete && Record.Def && Record.Def->MaxThrust > 0.f)
		{
			SlewPerSec = FMath::Max(0.01f, Record.Def->ThrottleSlewPerSec);
			ReserveFraction = Record.Def->LiftControlReserveFraction;
			HoverDampingPerMS = Record.Def->LiftHoverDampingPerMS;
			DescentRateMS = FMath::Max(0.f, Record.Def->LiftDescentRateMS);
			break;
		}
	}
	const float ThrottleCeiling = ExoneerThrust::PilotThrottleCeiling(ReserveFraction);
	const FVector LiftAxisWorld = CockpitQuat.GetAxisZ();

	// Thrusters deliver MaxThrust * Throttle * PowerSupplyFraction, so the
	// allocator must price its moments at the derated figure. Crediting full
	// thrust made the model lie to itself exactly during the brownout that
	// ends every flight on a battery-only craft.
	const float SupplyFraction = FMath::Clamp(PowerSupplyFraction, 0.f, 1.f);

	// --- Gather the thruster set and the installed attitude hardware ---
	const FVector CentreOfMass = PhysicsRoot->GetCenterOfMass();
	TArray<ExoneerThrust::FTrimEffector, TInlineAllocator<16>> Trims;
	TArray<UThrusterModule*, TInlineAllocator<16>> ThrusterModules;
	TArray<UGyroModule*, TInlineAllocator<4>> Gyros;
	float TotalRatedTorqueNm = 0.f;
	float PerAxisCapacityNms = 0.f;
	FVector StoredMomentumLocal = FVector::ZeroVector;
	ExoneerAttitude::FLoopParams Loop;
	float DumpRatePerSec = 0.f;
	float DumpOnsetFraction = 1.f;
	float CommandRateCeilingRadS = 0.f;
	float CommandMomentumFraction = 0.f;
	float DumpReleaseFraction = 0.f;
	float SustainedTurnSeconds = 0.f;
	float OffloadTimeConstantSeconds = 1.f;
	float LevelRateRadS = 0.f;
	float BankCeilingRad = 0.f;

	// World-vertical lift the craft makes at collective 1 (N), and the lift-axis
	// share of each unit, both needed by the governor below. A craft banked 30
	// degrees needs 15 percent more valve to hold altitude and gets asked for it.
	float HoverLiftScaleN = 0.f;
	float CraftLiftScaleN = 0.f;
	TArray<float, TInlineAllocator<16>> AlongLiftPerUnit;

	for (const TPair<int32, TObjectPtr<UVehicleModule>>& Pair : Modules)
	{
		if (UGyroModule* Gyro = Cast<UGyroModule>(Pair.Value.Get()))
		{
			Gyros.Add(Gyro);
			TotalRatedTorqueNm += Gyro->RatedTorqueNm;
			PerAxisCapacityNms += Gyro->MomentumCapacityNms;
			StoredMomentumLocal += Gyro->GetStoredMomentumLocal();
			// Loop constants come off the block definition, because the
			// attitude computer ships inside the gyro. One definition exists
			// today; if a second grade is ever authored, decide here which of
			// the two controllers the vehicle runs rather than letting map
			// order pick.
			if (const UVehicleBlockDefinitionDataAsset* Def = Gyro->FindDef())
			{
				Loop.SettleTimeSeconds = FMath::Max(0.05f, Def->AttitudeSettleTimeSeconds);
				Loop.DampingRatio = FMath::Max(0.1f, Def->AttitudeDampingRatio);
				DumpRatePerSec = FMath::Max(0.f, Def->MomentumDumpRatePerSec);
				DumpOnsetFraction = FMath::Clamp(Def->MomentumDumpOnsetFraction, 0.f, 1.f);
				CommandRateCeilingRadS = FMath::DegreesToRadians(FMath::Max(0.f, Def->AttitudeCommandRateCeilingDegPerSec));
				CommandMomentumFraction = FMath::Clamp(Def->AttitudeCommandMomentumFraction, 0.f, 1.f);
				DumpReleaseFraction = FMath::Clamp(Def->MomentumDumpReleaseFraction, 0.f, 1.f);
				SustainedTurnSeconds = FMath::Max(0.f, Def->AttitudeSustainedTurnSeconds);
				OffloadTimeConstantSeconds = FMath::Max(0.05f, Def->AttitudeOffloadTimeConstantSeconds);
				LevelRateRadS = FMath::DegreesToRadians(FMath::Max(0.f, Def->AttitudeLevelRateDegPerSec));
				BankCeilingRad = FMath::DegreesToRadians(FMath::Clamp(Def->AttitudeBankCeilingDeg, 0.f, 180.f));
			}
			continue;
		}
		UThrusterModule* Thruster = Cast<UThrusterModule>(Pair.Value.Get());
		const FVehicleBlockRecord* Record = Thruster ? Thruster->FindRecord() : nullptr;
		if (!Thruster || !Record || !Record->Def || Record->Def->MaxThrust <= 0.f)
		{
			continue;
		}
		const FTransform BlockTransform = GetBlockWorldTransform(*Record);
		// The jet, not the aim. A canted nozzle leaves the block a few degrees
		// off its own -X face, which is where a craft made of parallel lift
		// thrusters gets its yaw authority from.
		const FVector DirWorld = BlockTransform.TransformVectorNoScale(
			ExoneerThruster::LocalThrustDirection(Record->Def->NozzleCantDeg)).GetSafeNormal();
		const FVector ArmMetres = (BlockTransform.GetLocation() - CentreOfMass) / ExoneerUnits::CmPerM;
		const float DeliverableThrustN = Record->Def->MaxThrust * SupplyFraction;
		const float AlongLift = static_cast<float>(FMath::Max(0.0, FVector::DotProduct(DirWorld, LiftAxisWorld)));
		// SIGNED, deliberately. This feeds the hover governor's lift scale, and
		// the whole question past 90 degrees of bank is whether opening the
		// valve raises the craft or drives it into the ground. Clamped at zero
		// the sum could only ever answer "raises", so the governor froze the
		// valve at the ceiling while the craft was inverted and the six lift
		// nozzles pointed 21.1 kN at the ground. See HoverCollective.
		const float AlongWorldUp = static_cast<float>(FVector::DotProduct(DirWorld, FVector::UpVector));

		ExoneerThrust::FTrimEffector Effector;
		Effector.MomentPerUnitThrottle = FVector::CrossProduct(ArmMetres, DirWorld * DeliverableThrustN);
		Effector.ForcePerUnitThrottle = DirWorld * DeliverableThrustN;
		Effector.LiftPerUnitThrottle = DeliverableThrustN * AlongLift;
		// The pilot's horizontal demand only. The collective is added below,
		// once the governor has decided where the valve is going, and the whole
		// sum is held short of the stop so the trim path keeps authority at the
		// top of the lever (see PilotThrottleCeiling).
		Effector.BaseThrottle = FMath::Clamp(
			static_cast<float>(FMath::Max(0.0, FVector::DotProduct(DirWorld, HorizontalIntentWorld))) * HorizontalDemand,
			0.f, ThrottleCeiling);
		Effector.TrimFraction = FMath::Clamp(Record->Def->AttitudeTrimFraction, 0.f, 1.f);
		// The slew origin is last frame's bias; the target starts at zero, so
		// a valve with no trim work closes back to the pilot's setting at the
		// same authored rate it opened.
		Effector.PreviousTrim = Thruster->AttitudeTrim;
		Effector.Trim = 0.f;
		Trims.Add(MoveTemp(Effector));
		ThrusterModules.Add(Thruster);
		AlongLiftPerUnit.Add(AlongLift);
		// Priced at FULL rated thrust, deliberately, while the allocator above
		// is priced at what the brownout actually delivers. The governor's
		// feed-forward is the one place a derated price is a POSITIVE feedback:
		// less supply asked for a larger collective, a larger collective drew
		// more power (GetCurrentDraw scales with throttle), and the brownout
		// deepened itself. Priced at the rating, the feed-forward stops
		// chasing; the craft simply makes less lift than asked, sinks, and the
		// rate term opens the valve in response to a sink the pilot can see.
		HoverLiftScaleN += Record->Def->MaxThrust * AlongLift * AlongWorldUp;
		// The same figure along the craft's OWN lift axis: what it would make
		// level. The governor needs both, because "banked past holding weight"
		// and "cannot lift its own weight" are different failures and want
		// opposite answers - freeze for the first, run to the stop for the
		// second.
		CraftLiftScaleN += Record->Def->MaxThrust * AlongLift * AlongLift;
	}

	// --- The lift valve: up, down at a governed RATE, or hold ---
	const bool bLiftKeyHeld = bFlightControl && PilotInput.Move.Z > KINDA_SMALL_NUMBER;
	const bool bDescendHeld = bFlightControl && (PilotInput.HeldFlags & EPilotHeldFlags::Descend) != 0;
	float LiftTarget = 0.f;
	bLiftGovernorActive = false;
	bLiftGovernorPinned = false;
	bLiftInverted = false;
	bLiftDescending = false;
	if (bFlightControl && !bPilotInputStale)
	{
		if (bLiftKeyHeld)
		{
			LiftTarget = ThrottleCeiling;
		}
		else if (!bGroundContactLatched)
		{
			// HOVER, OR A GOVERNED DESCENT. The same governor runs in both
			// cases; the descend key only moves its vertical-speed reference.
			//
			// THE DESCEND KEY USED TO KILL THE VALVE, and that made the one
			// control a pilot needs on every approach unrecoverable: the craft
			// fell at a full 1 g while the governor could only arrest at
			// 0.19 g, so four seconds of held Ctrl reached 37 m/s and needed
			// 26 s and 526 m to stop, against landing damage that starts at
			// 8 m/s. Governed, the same four seconds reach 2.5 m/s and 7.9 m.
			// It also keeps the valve off its bottom stop, which is what keeps
			// the differential-thrust trim alive: Ctrl and W together were the
			// one state with no trim authority at all.
			const float GravityMS2 = FMath::Abs(
				GetWorld() ? GetWorld()->GetGravityZ() / ExoneerUnits::CmPerM : 9.81f);
			const float WeightN = PhysicsRoot->GetMass() * GravityMS2;
			const float TargetVerticalMS = bDescendHeld ? -DescentRateMS : 0.f;
			bLiftGovernorPinned = ExoneerThrust::IsHoverGovernorPinned(WeightN, HoverLiftScaleN,
				CraftLiftScaleN, ThrottleCeiling);
			bLiftInverted = ExoneerThrust::IsLiftInverted(HoverLiftScaleN);
			// ONE CALL, WHATEVER THE ATTITUDE. There used to be a special case
			// here: banked past holding weight AND asked to descend drove the
			// valve to its bottom stop. That reached the one state with NO trim
			// authority and NO momentum sink, in the air, with two keys a pilot
			// holds on every banked approach. Measured from 400 m with 4 s of D
			// and Ctrl held: the valve was at 0.000 by t = 3 s, the craft
			// free-fell at 1 g to -48.9 m/s and hit the ground at 38.8 m/s,
			// nearly five times the 8 m/s damage threshold. With the valve shut
			// every lift unit has zero DOWN-travel, so TrimBoundMin is 0 on all
			// six, the force null cannot balance an all-positive bias,
			// EnforceForceNeutralTrim zeroes every trim, and a saturated axis
			// then has no sink at all until touchdown.
			//
			// The governor already answers the question the pilot asked. A craft
			// banked out of authority is sinking anyway, so the frozen valve IS
			// the descent; inside authority the descend key is a bounded rate.
			// The same 4 s of D and Ctrl now: the valve floors at 0.530, holds
			// 0.880, the sink is -2.48 m/s, and releasing both keys levels off
			// 13.6 m lower in 3.9 s.
			LiftTarget = ExoneerThrust::HoverCollective(WeightN, HoverLiftScaleN, CraftLiftScaleN,
				static_cast<float>(GetVelocity().Z / ExoneerUnits::CmPerM), TargetVerticalMS,
				HoverDampingPerMS, ThrottleCeiling, LiftCollective);
			bLiftGovernorActive = LiftTarget > KINDA_SMALL_NUMBER;
			bLiftDescending = bDescendHeld;
		}
	}
	// The valve slews at the thruster's authored rate in BOTH directions.
	LiftCollective = ExoneerThrust::AdvanceCollective(LiftCollective, LiftTarget, DeltaSeconds, SlewPerSec);

	// Now the collective is known, finish the pilot's base throttles.
	for (int32 Index = 0; Index < Trims.Num(); ++Index)
	{
		Trims[Index].BaseThrottle = FMath::Clamp(
			Trims[Index].BaseThrottle + AlongLiftPerUnit[Index] * LiftCollective, 0.f, ThrottleCeiling);
	}

	// --- Attitude: roll and pitch HELD, yaw on rate, level on release ---
	// The stick asks for a body rate. In yaw that rate is the command, full
	// stop. In roll and pitch it drives an attitude REFERENCE which, released,
	// slews back to level - because a thrust vehicle has no restoring moment
	// about its own centre of mass, so any bank a bump leaves is a permanent
	// sideways acceleration and nothing else in the machine can end it.
	//
	// Nothing can wind up: the reference is DROPPED whenever the pilot is not
	// flying an airborne craft and re-seeded from the hull attitude when he
	// is, so it cannot integrate against the suspension and then release a
	// banked error the instant the wheels leave the ground. And the loop is a
	// cascade, not a spring - the reference error becomes a rate command
	// bounded by the same ceiling the stick has - so "let go and it stops
	// rotating" still holds, and the momentum a return to level costs is
	// exactly the momentum the rate budget already paid for.
	//
	// THE GATE IS A PILOT ABOARD, and nothing else. It is deliberately NOT
	// wheel contact: a contact gate cut the rate null the instant one wheel
	// touched, which took away damping the owner already had, and it answered
	// the ground question by accident. The answer on purpose is that the triad
	// assists in BOTH modes - a reaction wheel really does resist rotation
	// while the hull sits on its tyres, it is the pilot's instinctive "make it
	// stop", and the ground bleed in UGyroModule keeps whatever it stores from
	// being permanent. What Ground mode gates is THRUST.
	const FQuat BodyQuat = GetActorQuat();
	const FVector BodyRateLocal = BodyQuat.UnrotateVector(PhysicsRoot->GetPhysicsAngularVelocityInRadians());
	FVector AttitudeTorqueLocal = FVector::ZeroVector;

	Loop.InertiaKgM2 = GetBodyInertiaKgM2();
	Loop.HullAngularDamping = FMath::Max(0.f, PhysicsRoot->GetAngularDamping());

	const bool bAttitudeActive = PilotPawn != nullptr && Gyros.Num() > 0
		&& TotalRatedTorqueNm > 0.f && !Loop.InertiaKgM2.IsNearlyZero();
	FVector CommandRateLocal = FVector::ZeroVector;
	if (bAttitudeActive)
	{
		// Rotate intent is (pitch, yaw, roll) about the cockpit's right, up and
		// forward axes, and it exists only in Flight mode - in Ground mode the
		// same keys drive and steer, so an airborne rover gets a pure rate
		// null. The rate ceiling is what the momentum budget can actually pay
		// for, including the cost of HOLDING the rate, so a heavier hull turns
		// slower and a second gyro makes the same hull turn faster.
		//
		// THE LIMIT IS RESOLVED ABOUT THE COCKPIT AXIS IT IS APPLIED TO. The
		// previous form indexed a per-body-axis vector by axis NUMBER and then
		// used it about a cockpit axis, which is only right when the cockpit is
		// mounted square: on the first craft a player builds with a rotated
		// seat the pitch ask was scaled by the roll axis budget.
		const FVector RateLimitLocal = ExoneerAttitude::MaxCommandRateRadS(
			Loop, PerAxisCapacityNms, CommandMomentumFraction, CommandRateCeilingRadS, SustainedTurnSeconds);

		// ATTITUDE HOLD ON ROLL AND PITCH, RATE ON YAW, and the hold is a
		// CASCADE: the reference error becomes a rate command bounded by the
		// same ceiling the stick has, and the single rate loop below tracks it.
		// One gain set, one bound, and the momentum a return to level costs is
		// exactly the momentum the budget already paid for.
		//
		// Held roll and pitch are the whole answer to "it feels so weird to
		// fly". A thrust vehicle has no restoring moment about its own centre
		// of mass, so a 20 degree bump was a permanent 3.6 m/s^2 sideways
		// acceleration with the pilot holding nothing. Yaw is deliberately not
		// held: a locked heading is a compass nobody asked for.
		//
		// The reference is DROPPED whenever the pilot is not flying an airborne
		// craft, and re-seeded from the hull attitude when he is. That is the
		// freeze the previous pass wanted: a reference cannot integrate while
		// the suspension is holding the hull and then release a banked error
		// the instant the wheels leave the ground, because on the ground there
		// is no reference at all.
		const bool bHoldActive = bFlightControl && !bPilotInputStale && !bGroundContactLatched;
		FVector StickRateLocal = FVector::ZeroVector;
		if (bFlightControl)
		{
			const FVector Rotate = PilotInput.Rotate.BoundToCube(1.f);
			const FVector CockpitAxes[3] = { CockpitQuat.GetAxisX(), CockpitQuat.GetAxisY(), CockpitQuat.GetAxisZ() };
			const float Deflection[3] = { Rotate.Z, Rotate.X, Rotate.Y };   // roll, pitch, yaw
			FVector CommandRateWorld = FVector::ZeroVector;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				const float Limit = ExoneerAttitude::MaxCommandRateAboutAxisRadS(Loop,
					BodyQuat.UnrotateVector(CockpitAxes[Axis]), PerAxisCapacityNms,
					CommandMomentumFraction, CommandRateCeilingRadS, SustainedTurnSeconds);
				CommandRateWorld += CockpitAxes[Axis] * (Deflection[Axis] * Limit);
			}
			StickRateLocal = BodyQuat.UnrotateVector(CommandRateWorld);
		}

		if (!bHoldActive)
		{
			bAttitudeReferenceValid = false;
			CommandRateLocal = StickRateLocal;
		}
		else
		{
			// The leash is derived, not authored: the reference never sits
			// further from the hull than the outer loop can ask for at full
			// rate, which is exactly RateLimit * SettleTime.
			const FVector MaxErrorRad = ExoneerAttitude::MaxTrackableErrorRad(Loop, RateLimitLocal);
			if (!bAttitudeReferenceValid)
			{
				AttitudeReference = BodyQuat;
				bAttitudeReferenceValid = true;
			}
			const FVector RollPitchCommand(StickRateLocal.X, StickRateLocal.Y, 0.0);
			AttitudeReference = ExoneerAttitude::AdvanceReference(AttitudeReference, BodyQuat,
				RollPitchCommand, DeltaSeconds, MaxErrorRad);
			if (RollPitchCommand.IsNearlyZero())
			{
				// Stick released: back to level, heading untouched.
				AttitudeReference = ExoneerAttitude::LevelReference(AttitudeReference, LevelRateRadS * DeltaSeconds);
			}
			// Yaw is on rate, so nothing is ever held about the craft's own up
			// axis - re-released every frame, after the level slew, because
			// levelling can otherwise walk the heading.
			AttitudeReference = ExoneerAttitude::ReleaseBodyAxis(AttitudeReference, BodyQuat, 2);
			// THE BANK CEILING, and it is the other half of the powered-dive
			// fix. The leash above bounds the reference against the HULL,
			// which is anti-windup and says nothing about where the hull ends
			// up: A/D is FULL-DEFLECTION roll, so 3 s of one held key rolled
			// the hull to 53.9 degrees, 5 s to 93.2 degrees and 10 s to 168.7,
			// where the lift valve was pointing 21.3 kN at the ground - 2.2 g
			// of downward acceleration once weight is added. The limiter
			// refuses to COMMAND an attitude the craft cannot hold its weight
			// at - 30 degrees against the shipped rover's 32.0 degree
			// hold-weight angle - and applies no torque of its own, so a
			// collision or a slope can still put the hull anywhere and the
			// reference then flies it back. Bounded, 10 s of held D costs
			// 0.1 m of altitude instead of 288 m.
			AttitudeReference = ExoneerAttitude::LimitReferenceTilt(AttitudeReference, BankCeilingRad);
			AttitudeReference = ExoneerAttitude::LeashReference(AttitudeReference, BodyQuat, MaxErrorRad);

			FVector ErrorLocal = ExoneerAttitude::AttitudeErrorBody(BodyQuat, AttitudeReference);
			ErrorLocal.Z = 0.0;
			const float SettleTime = FMath::Max(Loop.SettleTimeSeconds, KINDA_SMALL_NUMBER);
			CommandRateLocal = FVector(
				FMath::Clamp(ErrorLocal.X / SettleTime, -RateLimitLocal.X, RateLimitLocal.X),
				FMath::Clamp(ErrorLocal.Y / SettleTime, -RateLimitLocal.Y, RateLimitLocal.Y),
				StickRateLocal.Z);
		}

		const FVector Kd = ExoneerAttitude::RateGain(Loop);
		AttitudeTorqueLocal = ((CommandRateLocal - BodyRateLocal) * Kd).BoundToCube(TotalRatedTorqueNm);
	}
	else
	{
		bAttitudeReferenceValid = false;
	}

	// --- Allocation ---
	// 1. THE TRIAD OWNS EVERY ATTITUDE TRANSIENT; THRUST OWNS EVERYTHING THAT
	//    DOES NOT DECAY. Differential thrust is a poor transient actuator - its
	//    authority is a hidden function of the throttle setting, exactly zero
	//    at the bottom of the travel, and a valve that moves for a transient
	//    moves the altitude with it - so a transient reaches it only through
	//    the offload lag in 4, which passes about 15 percent of a
	//    quarter-second input and all of a multi-second hold.
	//
	//    Rotor momentum is a bank account with a floor, so anything that does
	//    not decay MUST be paid by thrust: the standing moment in 2 and the
	//    rate-hold torque in 4. Whether thrust can pay is a property of the
	//    craft's geometry - lift thrusters pointing along the hull's own up
	//    axis make no yaw moment whatsoever, which is why the shipped rover's
	//    nozzles are canted - and where it cannot, the visor says so in
	//    seconds rather than the axis dying silently.
	//
	// 2. THE STANDING MOMENT, fed forward and cancelled by thrust. This is the
	//    defect that killed the shipped rover 1.3 s into forward flight: a
	//    thrust layout whose net moment about the centre of mass is non-zero
	//    makes a moment that NEVER ENDS. The rate law used to discover it
	//    through the rate error and hold it with rotor torque, which fills the
	//    momentum store at the moment's own magnitude per second - 1224 N*m
	//    against 1600 N*m*s took pitch out in 1.3 s and nothing could then stop
	//    the craft rotating. Thrust cancels thrust for free and for ever;
	//    rotor momentum is a bank account with a floor.
	//
	//    The moment is computed from the pilot's OWN base throttles about the
	//    MEASURED centre of mass, so it is right for any craft a player builds,
	//    not just for a layout somebody hand-balanced.
	const FVector StandingMomentLocal = BodyQuat.UnrotateVector(ExoneerThrust::StandingMomentNm(Trims));

	// 3. What the triad can ACTUALLY deliver, which is what the trim path's
	//    headroom must be measured from. The ask is the rate law's command PLUS
	//    the standing moment the triad has to hold if thrust does not; a rotor
	//    already at its stop delivers none of it in the winding direction.
	//    Measuring headroom from the raw ask read zero exactly when an axis was
	//    saturated, so the one path that could have recovered it throttled
	//    itself to nothing at the moment of failure.
	const FVector TriadAskLocal = (AttitudeTorqueLocal - StandingMomentLocal).BoundToCube(TotalRatedTorqueNm);
	const FVector DeliverableAskLocal = ExoneerAttitude::ApplySaturation(
		TriadAskLocal, StoredMomentumLocal, PerAxisCapacityNms);
	float UnwindHeadroomNm[3] = { 0.f, 0.f, 0.f };
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		UnwindHeadroomNm[Axis] = FMath::Max(0.f,
			TotalRatedTorqueNm - FMath::Abs(static_cast<float>(DeliverableAskLocal[Axis])));
	}

	GyroSaturation01 = PerAxisCapacityNms > KINDA_SMALL_NUMBER
		? FMath::Clamp(static_cast<float>(StoredMomentumLocal.GetAbsMax()) / PerAxisCapacityNms, 0.f, 1.f)
		: 0.f;

	// 4. THE SUSTAINED PART OF THE ATTITUDE COMMAND, offloaded to thrust. This
	//    is the defect the previous pass left open, and it is the same defect
	//    as the standing moment wearing a different hat. A hull with angular
	//    damping D needs D*I*w of torque to STAY at rate w, for ever: 120 N*m
	//    in yaw at the old 30 deg/s ceiling, which a reaction wheel pays at
	//    120 N*m*s every second. The rate ceiling now budgets a few seconds of
	//    that (AttitudeSustainedTurnSeconds); everything past it has to come
	//    from thrust, or one continuous 202 degree turn ends the axis.
	//
	//    The split is a first-order lag, so a stick TRANSIENT still never
	//    moves a valve - at the authored 1.5 s a quarter-second input passes
	//    about 15 percent - while a multi-second hold passes all of it. And
	//    because the whole trim vector is force-nulled below, none of it moves
	//    the altitude or the ground track.
	//
	// TWO GATES, because the two jobs are paid for by different things.
	//
	// THE STANDING MOMENT IS GATED ON THE VALVE BEING OPEN, ground contact or
	// not. It used to be gated on being airborne, and the lift key opens the
	// valve to the 0.90 ceiling regardless of the ground latch - so through
	// the whole ground roll the standing moment was real - 578 N*m of pitch and
	// 123 N*m of roll at the 0.90 ceiling - and nothing cancelled it:
	// GyroTorqueLocal = Attitude - Standing fed the whole thing into the rotors
	// at 578 N*m*s per second against an authored ground bleed of 0.25/s, whose
	// equilibrium is 578 / 0.25 = 2311 N*m*s and therefore ABOVE the 1600 N*m*s
	// cap, so the pitch axis saturates outright on any craft that lingers with
	// the valve open. Measured: the shipped rover broke ground in
	// 0.75 s having already spent 287 N*m*s of pitch, 18 percent of the
	// envelope, and never got it back - the residual is zero once airborne so
	// nothing refills it, and the 0.8 dump onset means nothing unwinds it
	// either. Held on the pad it reached 52 percent at 2 s, 63 at 2.5 s, 81 at
	// 3.5 s and saturated at 6 s. Gated on the valve instead, every one of
	// those dwells spends 0 N*m*s.
	//
	// It is safe on the ground precisely because the bias is force-nulled: it
	// changes no net force, only the moment, so it cannot lift a parked rover
	// or shift its weight onto one axle.
	const bool bPilotFlyingLive = bFlightControl && !bPilotInputStale;
	const bool bStandingTrimActive = bPilotFlyingLive
		&& (LiftCollective > KINDA_SMALL_NUMBER || HorizontalDemand > KINDA_SMALL_NUMBER);
	// The RATE offload and the desaturation dump stay gated on airborne: a
	// hull on its tyres has the ground as an external torque and needs no
	// thrust to hold a rate or to unwind, and UGyroModule's ground bleed is
	// already the sink there.
	const bool bTrimActive = bPilotFlyingLive && !bGroundContactLatched;
	SustainedAttitudeTorqueLocal = ExoneerAttitude::AdvanceSustainedTorque(
		SustainedAttitudeTorqueLocal,
		bTrimActive ? AttitudeTorqueLocal : FVector::ZeroVector,
		DeltaSeconds, OffloadTimeConstantSeconds);

	// 5. Desaturation rides on top, and only past the authored onset.
	//    Commanding along +h drives h down; the triad commands the exact
	//    opposite of the thrust pair, so attitude does not move while the
	//    rotors unwind. THAT mirroring is why the unwind - and only the unwind
	//    - is capped at the triad's remaining headroom: an unwind the triad
	//    cannot mirror is a disturbance the pilot did not ask for. The standing
	//    moment and the offload need no such cap, because there the thrust is
	//    REPLACING rotor torque rather than being cancelled by it.
	//
	//    The onset has HYSTERESIS. Without it the unwind stopped the instant
	//    saturation fell back below the onset and the axis parked at 80 percent
	//    of its envelope for the rest of the flight.
	FVector TrimRequestLocal = FVector::ZeroVector;
	bDesaturatingLatched = ExoneerThrust::ShouldDesaturate(GyroSaturation01, DumpOnsetFraction,
		DumpReleaseFraction, DumpRatePerSec, bDesaturatingLatched) && !StoredMomentumLocal.IsNearlyZero();
	if (bStandingTrimActive)
	{
		TrimRequestLocal -= StandingMomentLocal;
	}
	if (bTrimActive)
	{
		TrimRequestLocal += SustainedAttitudeTorqueLocal;
		if (bDesaturatingLatched)
		{
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				const float Unwind = FMath::Clamp(static_cast<float>(StoredMomentumLocal[Axis] * DumpRatePerSec),
					-UnwindHeadroomNm[Axis], UnwindHeadroomNm[Axis]);
				TrimRequestLocal[Axis] -= Unwind;
			}
		}
	}

	// THE AXES A TRIM MAY NOT MOVE THE CRAFT ALONG, in priority order. Lift
	// first, because an altitude that moves on its own is unflyable; then the
	// cockpit's forward axis, because a bias on a forward-facing unit has no
	// lift component at all and was therefore free to the allocator while
	// changing the ground track by up to 400 N. The lateral axis is not asked
	// for: on the shipped rover no bias can cancel its 103 N*m roll standing
	// moment without some side force, and dropping the moment is worse than
	// carrying the few tens of newtons that come with it (peak 35 N over a
	// 150 s sortie). AllocateForceNeutralTrim drops axes from the back if the
	// geometry cannot hold them, and never drops lift.
	const FVector TrimNullAxes[2] = { LiftAxisWorld, CockpitQuat.GetAxisX() };
	ExoneerThrust::AllocateForceNeutralTrim(Trims, BodyQuat.RotateVector(TrimRequestLocal),
		TrimNullAxes, static_cast<int32>(UE_ARRAY_COUNT(TrimNullAxes)));

	// The valve is an actuator, not a switch: rate-limit the bias, hold it
	// inside the travel each valve actually has, then remove whatever net
	// force it would add - so a trim is a pure moment and neither the pilot's
	// altitude nor his ground track moves while it works.
	//
	// The clamp is not decoration. The slew is measured from last frame's
	// committed bias and the bound moves with the collective, so a valve
	// slewing toward a smaller bound could carry a bias up to SlewPerSec*dt
	// outside it - 0.033 at 60 fps, about 130 N and 100 N*m - and the commit
	// clamp then broke both guarantees silently for that frame.
	ExoneerThrust::SlewTrimTowardsRequest(Trims, DeltaSeconds, SlewPerSec);
	ExoneerThrust::ClampTrimsToBounds(Trims);
	ExoneerThrust::EnforceForceNeutralTrim(Trims, TrimNullAxes, static_cast<int32>(UE_ARRAY_COUNT(TrimNullAxes)));
	const FVector TrimTorqueLocal = BodyQuat.UnrotateVector(ExoneerThrust::DeliveredTrimTorqueNm(Trims));

	// 6. The triad's command, with BOTH thrust moments fed forward. Hull torque
	//    is standing + trim + gyro, and the rate law wants it to equal the PD
	//    command, so the gyro takes the difference. Two cases fall straight out
	//    of the one line:
	//      - standing moment fully trimmed: the two cancel, the gyro is left
	//        with the PD command alone, and the rotors pay for transients only;
	//      - desaturating: the trim is the unwind, the gyro commands its exact
	//        opposite, attitude does not move and the rotors unwind.
	//    Subtracting the trim WITHOUT subtracting the standing moment - which
	//    is what the previous pass did - simply moved the standing moment into
	//    the gyro command and the rotors filled at the same rate as before.
	const FVector GyroTorqueLocal = (AttitudeTorqueLocal - StandingMomentLocal - TrimTorqueLocal)
		.BoundToCube(TotalRatedTorqueNm);

	// What the thrust group could NOT cancel is what the rotors are paying for,
	// in N*m and therefore in N*m*s per second. This is the visor's countdown
	// and the one honest answer to "why did my craft die": a build whose thrust
	// does not balance about its own centre of mass spends momentum it has no
	// way to earn back in the air.
	// Low-passed over two seconds. This is a DISPLAY filter, not a control
	// constant: the number the pilot needs is the SUSTAINED spend, and the
	// valve takes a fraction of a second to slew onto a new trim setting, so
	// the instantaneous residual spikes to a few tens of N*m every time the
	// collective moves. Averaging hides the slew and keeps a standing moment.
	{
		// MEASURED AGAINST THE REQUEST THAT WAS ACTUALLY MADE. Reading
		// |Standing + Trim| instead was wrong in the one state the warning
		// exists for: while desaturating the request is -Standing - Unwind, so
		// a SUCCESSFUL unwind left that expression equal to -Unwind - up to
		// 560 N*m - and the visor screamed "UNBALANCED, 0 s of gyro" in
		// pulsing red at exactly the moment the recovery mechanism was
		// working. A warning that fires when nothing is wrong is a warning the
		// pilot learns to ignore before the one true instance of it.
		// Only while the trim path is actually running: with no request made
		// there is no un-met request, and the bias decaying back to neutral
		// after touchdown is not an imbalance to warn about.
		const FVector Unmet = (bStandingTrimActive || bTrimActive)
			? (TrimRequestLocal - TrimTorqueLocal)
			: FVector::ZeroVector;
		const float Instant = static_cast<float>(Unmet.GetAbsMax());
		constexpr float FilterSeconds = 2.f;
		const float Alpha = FMath::Clamp(DeltaSeconds / FilterSeconds, 0.f, 1.f);
		UntrimmedStandingMomentNm += (Instant - UntrimmedStandingMomentNm) * Alpha;
		{
			// WHICH axis, or the pilot is told a number and nothing to stop
			// doing. Same rule the momentum readout uses.
			const FVector Absolute = Unmet.GetAbs();
			UntrimmedWorstAxis = (Absolute.X >= Absolute.Y && Absolute.X >= Absolute.Z) ? 0
				: ((Absolute.Y >= Absolute.Z) ? 1 : 2);
		}
		// THE STANDING SIDE FORCE, which no other readout can show. The trim
		// path nulls the net TRIM force and never the base throttle's own, and
		// flight has no lateral thrust command, so a build whose lift nozzles
		// are all toed the same way accelerates sideways at 1.03 m/s^2 for
		// ever with an empty momentum store and a green visor. Measured on the
		// pilot's own base throttles, about the SEAT's right axis, because
		// that is the direction the pilot would have to counter and cannot.
		{
			FVector BaseForce = FVector::ZeroVector;
			for (const ExoneerThrust::FTrimEffector& Effector : Trims)
			{
				BaseForce += Effector.ForcePerUnitThrottle * Effector.BaseThrottle;
			}
			const float Side = static_cast<float>(FVector::DotProduct(BaseForce, CockpitQuat.GetAxisY()));
			StandingSideForceN += (Side - StandingSideForceN) * Alpha;
		}
	}

	// Which axis the rotor store is worst on, for the visor. A percentage with
	// no axis name tells the pilot something is wrong and nothing about what to
	// stop doing.
	{
		const FVector Absolute = StoredMomentumLocal.GetAbs();
		GyroWorstAxis = (Absolute.X >= Absolute.Y && Absolute.X >= Absolute.Z) ? 0
			: ((Absolute.Y >= Absolute.Z) ? 1 : 2);
	}

	// --- Commit ---
	const FVector GyroTorqueWorld = BodyQuat.RotateVector(GyroTorqueLocal);
	const float PerGyro = Gyros.Num() > 0 ? 1.f / static_cast<float>(Gyros.Num()) : 0.f;
	for (UGyroModule* Gyro : Gyros)
	{
		Gyro->CommandTorqueWorldNm = GyroTorqueWorld * PerGyro;
	}
	for (int32 Index = 0; Index < Trims.Num(); ++Index)
	{
		const ExoneerThrust::FTrimEffector& Effector = Trims[Index];
		UThrusterModule* Thruster = ThrusterModules[Index];
		Thruster->AttitudeTrim = Effector.Trim;
		const float NewThrottle = FMath::Clamp(Effector.BaseThrottle + Effector.Trim, 0.f, 1.f);
		Thruster->Throttle = NewThrottle;

		// Mirror throttle into the record for client VFX, quantized.
		if (FVehicleBlockRecord* Record = FindMutableRecord(Thruster->GetBlockInstanceId()))
		{
			if (!FMath::IsNearlyEqual(Record->StateScalar, NewThrottle, 0.01f))
			{
				Record->StateScalar = NewThrottle;
				BlockList.MarkItemDirty(*Record);
			}
		}
	}

	// Gyro torque itself is applied in UGyroModule::TickModule, which runs one
	// step after this router - the same split thrusters already use. A sleeping
	// body will not wake from AddTorqueInRadians alone, so wake it here when
	// the pilot asks for attitude.
	if (!GyroTorqueWorld.IsNearlyZero() && !PhysicsRoot->RigidBodyIsAwake())
	{
		PhysicsRoot->WakeAllRigidBodies();
	}
}

void AVehicleConstruct::ServerRouteDrive(float DeltaSeconds)
{
	// Route ground-drive commands to wheel modules. Flight mode zeroes drive
	// and service brake (the parking brake still applies through the flag),
	// so a rover with thrusters cannot double-drive.
	const bool bGround = ControlMode == EPilotControlMode::Ground;
	const float ThrottleTarget = bGround && PilotPawn ? PilotInput.Throttle : 0.f;
	const float SteerCmd = bGround && PilotPawn ? PilotInput.Steer : 0.f;
	const float BrakeCmd = bGround && PilotPawn ? PilotInput.Brake : 0.f;

	// Ramp the binary keyboard throttle so W taps creep and holds build up.
	const float RampRate = FMath::Abs(ThrottleTarget) > FMath::Abs(CurrentDriveThrottle)
		? DriveThrottleRampUpPerSec
		: DriveThrottleRampDownPerSec;
	CurrentDriveThrottle += FMath::Clamp(ThrottleTarget - CurrentDriveThrottle,
		-RampRate * DeltaSeconds, RampRate * DeltaSeconds);
	const float ThrottleCmd = CurrentDriveThrottle;

	// Ackermann reference frame: the active cockpit's local frame (actor axes
	// without one, e.g. before the first seating).
	FQuat FrameLocal = FQuat::Identity;
	if (const FVehicleBlockRecord* Cockpit = FindRecord(ActiveCockpitId))
	{
		FrameLocal = ExoneerVehicleOrientation::GetQuat(Cockpit->Orientation);
	}
	const FVector ForwardLocal = FrameLocal.GetAxisX();
	const FVector RightLocal = FrameLocal.GetAxisY();

	struct FWheelEntry
	{
		UWheelModule* Module = nullptr;
		const FVehicleWheelSpec* Spec = nullptr;
		float Longitudinal = 0.f;   // m, along cockpit forward
		float Lateral = 0.f;        // m, along cockpit right
		float DesiredSteer = 0.f;   // rad, before the shared limit scale
	};
	TArray<FWheelEntry, TInlineAllocator<12>> Wheels;
	float NonSteerableSum = 0.f;
	int32 NonSteerableCount = 0;
	float MinLongitudinal = TNumericLimits<float>::Max();
	float LateralSum = 0.f;

	for (const TPair<int32, TObjectPtr<UVehicleModule>>& Pair : Modules)
	{
		UWheelModule* Wheel = Cast<UWheelModule>(Pair.Value.Get());
		if (!Wheel)
		{
			continue;
		}
		const FVehicleBlockRecord* Record = Wheel->FindRecord();
		if (!Record || !Record->Def)
		{
			continue;
		}
		FWheelEntry& Entry = Wheels.AddDefaulted_GetRef();
		Entry.Module = Wheel;
		Entry.Spec = &Record->Def->WheelSpec;
		const FVector CenterLocal = GetBlockLocalTransform(*Record).GetLocation() / ExoneerUnits::CmPerM;
		Entry.Longitudinal = FVector::DotProduct(CenterLocal, ForwardLocal);
		Entry.Lateral = FVector::DotProduct(CenterLocal, RightLocal);
		LateralSum += Entry.Lateral;
		MinLongitudinal = FMath::Min(MinLongitudinal, Entry.Longitudinal);
		if (!Entry.Spec->bSteerable)
		{
			NonSteerableSum += Entry.Longitudinal;
			++NonSteerableCount;
		}
	}
	if (Wheels.Num() == 0)
	{
		return;
	}

	// Rear-axle line: centroid of the non-steered wheels (all-steer rigs fall
	// back to the rear-most wheel). The Ackermann turn center sits on it.
	const float RearAxle = NonSteerableCount > 0 ? NonSteerableSum / NonSteerableCount : MinLongitudinal;

	// Track centreline: the mean lateral of the wheels themselves. Ackermann
	// geometry is defined about the VEHICLE's own longitudinal axis, and the
	// wheelbase below is already measured relative to the rear-axle line, so
	// the lateral term has to be relative too. Measuring it from the actor
	// pivot instead is correct only for a build that happens to be symmetric
	// about Y = 0, which a ladder chassis is not: the test rover's wheels sit
	// at -0.125 m and +1.125 m, so the turn centre was built 0.625 m off axis
	// and the same stick gave about fifty percent more effective steer one way
	// than the other. That asymmetry is felt long before it can be named.
	const float TrackCentreLateral = LateralSum / (float)Wheels.Num();

	float SteerableWheelbaseSum = 0.f;
	int32 SteerableCount = 0;
	for (const FWheelEntry& Entry : Wheels)
	{
		if (Entry.Spec->bSteerable)
		{
			SteerableWheelbaseSum += FMath::Max(Entry.Longitudinal - RearAxle, 0.f);
			++SteerableCount;
		}
	}
	const float MeanWheelbase = SteerableCount > 0
		? FMath::Max(SteerableWheelbaseSum / SteerableCount, 0.3f)
		: 0.3f;

	// Resolve Ackermann angles first, then scale the whole set to the limit.
	// Clamping each wheel independently (the old behaviour) broke the shared
	// turn centre at full lock: the inner wheel always wants more angle than
	// the outer, so it hit the clamp while the outer did not, and the two
	// wheels stopped pointing at the same centre - they fought each other and
	// scrubbed. Scaling preserves the geometry and just widens the turn.
	float LargestDemand = 0.f;
	float SteerLimit = TNumericLimits<float>::Max();
	for (FWheelEntry& Entry : Wheels)
	{
		const FVehicleWheelSpec& Spec = *Entry.Spec;
		if (!Spec.bSteerable)
		{
			Entry.DesiredSteer = 0.f;
			continue;
		}
		const float MaxSteer = FMath::DegreesToRadians(Spec.MaxSteerAngleDeg);
		SteerLimit = FMath::Min(SteerLimit, MaxSteer);
		const float MeanAngle = SteerCmd * MaxSteer;
		const float Wheelbase = FMath::Max(Entry.Longitudinal - RearAxle, 0.f);
		if (FMath::Abs(MeanAngle) < 0.017f || Wheelbase < 0.05f)
		{
			Entry.DesiredSteer = MeanAngle;
		}
		else
		{
			// True Ackermann from the layout: one turn center on the rear-axle
			// line; each steered wheel aims tangentially at it, so the inner
			// wheel steers tighter. Positive steer = right.
			const float Side = MeanAngle > 0.f ? 1.f : -1.f;
			const float CenterLateral = Side * MeanWheelbase / FMath::Tan(FMath::Abs(MeanAngle));
			const float Denominator = CenterLateral - (Entry.Lateral - TrackCentreLateral);
			Entry.DesiredSteer = FMath::Abs(Denominator) < 0.1f
				? Side * MaxSteer
				: FMath::Atan(Wheelbase / Denominator);
		}
		LargestDemand = FMath::Max(LargestDemand, FMath::Abs(Entry.DesiredSteer));
	}
	const float SteerScale = (LargestDemand > SteerLimit && LargestDemand > KINDA_SMALL_NUMBER)
		? SteerLimit / LargestDemand
		: 1.f;

	for (FWheelEntry& Entry : Wheels)
	{
		UWheelModule* Wheel = Entry.Module;
		const FVehicleWheelSpec& Spec = *Entry.Spec;
		Wheel->ThrottleCommand = Spec.bDriven ? ThrottleCmd : 0.f;
		Wheel->BrakeCommand = BrakeCmd;
		Wheel->bParkingBrake = bParkingBrakeEngaged;
		Wheel->TargetSlipCap = StockSlipCap;   // Shear Control talent lowers this to ~0.3 when talents land.
		Wheel->TargetSteerAngleRad = Entry.DesiredSteer * SteerScale;
	}
}

void AVehicleConstruct::UpdateWheelVisuals(float DeltaSeconds)
{
	for (TPair<int32, TObjectPtr<UStaticMeshComponent>>& Pair : WheelVisuals)
	{
		UStaticMeshComponent* WheelMesh = Pair.Value.Get();
		const FVehicleBlockRecord* Record = FindRecord(Pair.Key);
		if (!WheelMesh || !Record || !Record->Def)
		{
			continue;
		}
		const FVehicleWheelSpec& Spec = Record->Def->WheelSpec;
		const FVehicleWheelStateItem* State = WheelStates.FindByBlockId(Pair.Key);
		const float SteerAngle = State ? State->GetSteerAngleRad() : 0.f;
		const float Omega = State ? State->GetOmegaRadS() : 0.f;
		// The replicated compression is normalised over the strut's WHOLE
		// range, bump stop included - the same range the solver clamps to - so
		// the drawn hub matches the solved hub at every compression.
		const float CompressionM = State ? State->GetCompression01() * (Spec.TravelM + Spec.BumpStopTravelM) : 0.f;

		// Spin is integrated locally from the replicated RATE - the position
		// never replicates and drift is invisible on a rolling wheel.
		float& SpinAngle = WheelSpinAngles.FindOrAdd(Pair.Key);
		SpinAngle = FMath::Fmod(SpinAngle + Omega * DeltaSeconds, 2.f * PI);

		// Mesh-frame scale: MeshRelativeTransform maps the mesh onto the wheel
		// frame (axle = block local Y), so the target size is pulled back
		// through its rotation - the Z-aligned engine cylinder scales on its
		// own axes even though it spins about Y after the roll.
		UStaticMesh* Mesh = WheelMesh->GetStaticMesh();
		const FBox MeshBounds = Mesh ? Mesh->GetBoundingBox() : FBox(FVector(-50.f), FVector(50.f));
		const FVector MeshSize = MeshBounds.GetSize();
		const FVector TargetWheelFrame(2.f * Spec.RadiusM, Spec.WidthM, 2.f * Spec.RadiusM);
		const FVector TargetMeshFrame = Record->Def->MeshRelativeTransform.GetRotation()
			.UnrotateVector(TargetWheelFrame).GetAbs() * ExoneerUnits::CmPerM;
		const FVector Scale(
			MeshSize.X > 0.f ? TargetMeshFrame.X / MeshSize.X : 1.f,
			MeshSize.Y > 0.f ? TargetMeshFrame.Y / MeshSize.Y : 1.f,
			MeshSize.Z > 0.f ? TargetMeshFrame.Z / MeshSize.Z : 1.f);
		// A wheel spins about the centre of its own bounds, whatever the
		// authoring pivot is. Subtracting the bounds centre puts that centre on
		// the hub, exactly as BasePiece already does for a base-pivoted piece;
		// without it a hub-face or base pivot slides the whole wheel off its
		// axle the moment a real wheel mesh replaces the placeholder cylinder.
		const FVector MeshPivotOffset = -MeshBounds.GetCenter() * Scale;

		// Chain (applied left to right): mesh scale, mesh alignment, spin
		// about the axle (block local Y), steer about block local Z plus the
		// suspension drop along block local -Z, then the block's frame.
		const float HubDropUU = (Spec.RestLengthM - CompressionM) * ExoneerUnits::CmPerM;
		const FTransform ScaleXf(FQuat::Identity, MeshPivotOffset, Scale);
		const FTransform SpinXf(FQuat(FVector::YAxisVector, SpinAngle));
		const FTransform SteerAndDropXf(FQuat(FVector::ZAxisVector, SteerAngle), FVector(0.f, 0.f, -HubDropUU));
		const FTransform Relative = ScaleXf * Record->Def->MeshRelativeTransform * SpinXf * SteerAndDropXf * GetBlockLocalTransform(*Record);
		WheelMesh->SetRelativeTransform(Relative);
	}
}

FVehicleWheelStateItem& AVehicleConstruct::FindOrAddWheelStateItem(int32 BlockInstanceId)
{
	if (FVehicleWheelStateItem* Existing = WheelStates.FindByBlockId(BlockInstanceId))
	{
		return *Existing;
	}
	FVehicleWheelStateItem& Added = WheelStates.Items.AddDefaulted_GetRef();
	Added.BlockInstanceId = BlockInstanceId;
	WheelStates.MarkItemDirty(Added);
	return Added;
}

void AVehicleConstruct::RemoveWheelStateItem(int32 BlockInstanceId)
{
	const int32 Removed = WheelStates.Items.RemoveAll([BlockInstanceId](const FVehicleWheelStateItem& Item)
	{
		return Item.BlockInstanceId == BlockInstanceId;
	});
	if (Removed > 0)
	{
		WheelStates.MarkArrayDirty();
	}
}

bool AVehicleConstruct::IsAnyWheelInContact() const
{
	return WheelStates.Items.ContainsByPredicate(
		[](const FVehicleWheelStateItem& Item) { return Item.CompressionQ > 0; });
}

bool AVehicleConstruct::IsSupportedByGround() const
{
	// A loaded tyre answers it for free and exactly.
	if (IsAnyWheelInContact())
	{
		return true;
	}
	// Memoised for the rest of this frame: the sweep below is the expensive
	// part and three callers want the answer every tick.
	const uint64 Frame = GFrameCounter;
	if (GroundSupportFrame == Frame)
	{
		return bGroundSupportCached;
	}
	GroundSupportFrame = Frame;
	bGroundSupportCached = false;
	if (!PhysicsRoot || !PhysicsRoot->IsSimulatingPhysics())
	{
		return false;
	}
	// No wheels, or all of them off the ground: ask whether the HULL is
	// resting on something. This matters because the momentum sink reads this
	// predicate, and a reaction wheel unwinds against the CONTACT, not against
	// the tyre - a craft built with no wheel blocks has an empty wheel array
	// and used to have no sink at all, so one saturated axis was permanent.
	//
	// Cheap reject first: a hull falling faster than a hull can settle is not
	// resting on anything, so the sweep is skipped in the common airborne case.
	if (GetVelocity().Z < -static_cast<double>(CellSize))   // 0.25 m/s down
	{
		return false;
	}
	const FBoxSphereBounds Bounds = PhysicsRoot->Bounds;
	// The probe looks a fifth of a build cell below the footprint. That is a
	// property of the grid the vehicle is made of, not an authored tuning
	// value: it is the smallest gap that cannot hide a block.
	const float ProbeUU = CellSize * 0.2f;
	const FVector Extent(FMath::Max(Bounds.BoxExtent.X - 1.0, 1.0),
		FMath::Max(Bounds.BoxExtent.Y - 1.0, 1.0), 1.0);
	const FVector Start(Bounds.Origin.X, Bounds.Origin.Y, Bounds.Origin.Z - Bounds.BoxExtent.Z + 1.0);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ExoneerHullSupport), false, this);
	Params.AddIgnoredActor(PilotPawn.Get());
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(ECC_WorldStatic);
	Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
	FHitResult Hit;
	bGroundSupportCached = GetWorld() && GetWorld()->SweepSingleByObjectType(Hit, Start,
		Start - FVector(0.f, 0.f, ProbeUU), FQuat::Identity, Objects,
		FCollisionShape::MakeBox(Extent), Params);
	return bGroundSupportCached;
}

FVehicleDrivetrainSummary AVehicleConstruct::GetDrivetrainSummary() const
{
	FVehicleDrivetrainSummary Summary;
	Summary.SpeedMS = GetVelocity().Size() / ExoneerUnits::CmPerM;
	// SPEED alone is a magnitude, so a 30 m/s climb reads the same as level
	// flight. The pilot of a 1.3 TWR craft needs the vertical component and
	// the valve setting as separate numbers, or lift is guesswork.
	Summary.VerticalSpeedMS = GetVelocity().Z / ExoneerUnits::CmPerM;
	Summary.LiftFraction01 = FMath::Clamp(LiftCollective, 0.f, 1.f);
	Summary.bLiftGovernorActive = bLiftGovernorActive;
	Summary.bLiftGovernorPinned = bLiftGovernorPinned;
	Summary.bLiftDescending = bLiftDescending;
	Summary.GyroWorstAxis = GyroWorstAxis;
	Summary.bHasFuelCapacity = HasFuelCapacity();
	{
		// Bank and pitch of the SEAT, which is the frame the pilot's stick and
		// eyes are in. A craft that holds whatever attitude it is left in has
		// to show that attitude somewhere.
		const FVehicleBlockRecord* Seat = FindRecord(ActiveCockpitId);
		const FQuat SeatQuat = Seat
			? GetActorQuat() * ExoneerVehicleOrientation::GetQuat(Seat->Orientation)
			: GetActorQuat();
		const FRotator Attitude = SeatQuat.Rotator();
		Summary.BankDeg = static_cast<float>(Attitude.Roll);
		Summary.PitchDeg = static_cast<float>(Attitude.Pitch);
	}
	Summary.UntrimmedStandingMomentNm = UntrimmedStandingMomentNm;
	Summary.UntrimmedWorstAxis = UntrimmedWorstAxis;
	Summary.StandingSideForceN = StandingSideForceN;
	Summary.bLiftInverted = bLiftInverted;
	Summary.GyroMomentumCapacityNms = GetInstalledGyroMomentumCapacityNms();
	Summary.bParkingBrake = bParkingBrakeEngaged;
	Summary.bCanDrive = HasCompleteWheel();
	Summary.bCanFly = HasCompleteThruster();
	Summary.GyroTorqueNm = GetInstalledGyroTorqueNm();
	Summary.GyroSaturation01 = GyroSaturation01;
	bool bFirst = true;
	for (const FVehicleWheelStateItem& Item : WheelStates.Items)
	{
		++Summary.WheelCount;
		Summary.WorstSlipRatio = FMath::Max(Summary.WorstSlipRatio, Item.GetSlipRatioAbs());
		Summary.MaxSinkageM = FMath::Max(Summary.MaxSinkageM, Item.GetSinkageM());
		Summary.MinTirePressureKPa = bFirst
			? Item.GetTirePressureKPa()
			: FMath::Min(Summary.MinTirePressureKPa, Item.GetTirePressureKPa());
		bFirst = false;
		if (Item.CompressionQ > 0)
		{
			++Summary.WheelsInContact;
		}
	}
	Summary.MinTreadDepthMm = GetMinTreadDepthMm();
	Summary.MaxWindingTempC = GetMaxWindingTempC();
	Summary.bAnyWindingDerating = IsAnyWindingDerating();
	Summary.bAnyThermalCutout = HasThermalCutout();
	Summary.StoredFuelKg = GetStoredFuelKg();
	Summary.AscentTwr = GetAscentTwr();
	GetBatteryEnergy(Summary.StoredEnergyWs, Summary.EnergyCapacityWs);
	Summary.LastLandingSpeedMS = LastLandingSpeedMS;
	float HealthSum = 0.f;
	float HealthMax = 0.f;
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (Record.Phase == EConstructionPhase::Complete && Record.Def)
		{
			HealthSum += Record.Health;
			HealthMax += Record.Def->MaxHealth;
		}
	}
	Summary.HullHealth01 = HealthMax > 0.f ? HealthSum / HealthMax : 1.f;
	return Summary;
}

void AVehicleConstruct::GetBatteryEnergy(float& OutStoredWs, float& OutCapacityWs) const
{
	OutStoredWs = 0.f;
	OutCapacityWs = 0.f;
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (Record.Phase == EConstructionPhase::Complete && Record.Def && Record.Def->EnergyStorage > 0.f)
		{
			// The pilot panel reads what the pack can still hold, not what it
			// was rated for when it was new.
			const float CapacityJ = ExoneerMaintenance::EffectiveCapacityJ(
				Record.Def->EnergyStorage, Record.Condition.CapacityFade01);
			OutCapacityWs += CapacityJ;
			OutStoredWs += Record.StateScalar * CapacityJ;
		}
	}
}

bool AVehicleConstruct::HasCompleteWheel() const
{
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (Record.Phase == EConstructionPhase::Complete && Record.Def && Record.Def->bIsWheel)
		{
			return true;
		}
	}
	return false;
}

float AVehicleConstruct::GetInstalledGyroTorqueNm() const
{
	// Record-driven, never the server-only Modules map: GetDrivetrainSummary
	// feeds the HUD on both sides.
	float Total = 0.f;
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (Record.Phase == EConstructionPhase::Complete && Record.Def)
		{
			Total += FMath::Max(0.f, Record.Def->MaxGyroTorqueNm);
		}
	}
	return Total;
}

float AVehicleConstruct::GetInstalledGyroMomentumCapacityNms() const
{
	float Total = 0.f;
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (Record.Phase == EConstructionPhase::Complete && Record.Def)
		{
			Total += FMath::Max(0.f, Record.Def->GyroMomentumCapacityNms);
		}
	}
	return Total;
}

bool AVehicleConstruct::HasCompleteThruster() const
{
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (Record.Phase == EConstructionPhase::Complete && Record.Def && Record.Def->ModuleClass
			&& Record.Def->ModuleClass->IsChildOf(UThrusterModule::StaticClass()))
		{
			return true;
		}
	}
	return false;
}

void AVehicleConstruct::ServerTickModules(float DeltaSeconds)
{
	// Tick a snapshot; a module may clear pilot state but never mutates the map.
	TArray<TObjectPtr<UVehicleModule>> ToTick;
	Modules.GenerateValueArray(ToTick);
	for (const TObjectPtr<UVehicleModule>& Module : ToTick)
	{
		if (Module)
		{
			Module->TickModule(DeltaSeconds);
		}
	}
}

// --- Derived state (server + client) ---

void AVehicleConstruct::RebuildDerivedState()
{
	BlockList.OwnerConstruct = this;
	const bool bAuthority = HasAuthority();

	// 1. Cell map.
	CellToBlock.Reset();
	TArray<FIntVector> Cells;
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (!Record.Def)
		{
			continue;
		}
		ExoneerVehicleOrientation::GetOccupiedCells(Record.Origin, Record.Def->SizeInCells, Record.Orientation, Cells);
		for (const FIntVector& Cell : Cells)
		{
			CellToBlock.Add(Cell, Record.BlockInstanceId);
		}
	}

	// 2. Physics gate BEFORE box creation: the rigid body only simulates while
	// at least one COMPLETE block provides real collision, and welds only merge
	// into a body that is ALREADY simulating when the child attaches. On the
	// off->on flip every existing box is destroyed so the loop below recreates
	// them against the live body - otherwise the vehicle simulates with zero
	// collidable shapes and falls through the world.
	FVector FreezeLocation = GetActorLocation();
	FRotator FreezeRotation = GetActorRotation();
	bool bJustEnabledSim = false;
	if (bAuthority && PhysicsRoot)
	{
		bool bAnyComplete = false;
		for (const FVehicleBlockRecord& Record : BlockList.Blocks)
		{
			if (Record.Phase == EConstructionPhase::Complete)
			{
				bAnyComplete = true;
				break;
			}
		}
		const bool bWasSimulating = PhysicsRoot->IsSimulatingPhysics();
		if (bAnyComplete != bWasSimulating)
		{
			if (bAnyComplete)
			{
				// First Complete block: enable sim with gravity OFF so the
				// empty root cannot fall through the world in the gap before
				// welded boxes exist (that gap is why a just-finished ghost
				// "vanished" if the player held weld a moment too long).
				PhysicsRoot->SetEnableGravity(false);
				bJustEnabledSim = true;
			}
			PhysicsRoot->SetSimulatePhysics(bAnyComplete);
			for (TPair<int32, TObjectPtr<UBoxComponent>>& Pair : BlockBodies)
			{
				if (Pair.Value)
				{
					Pair.Value->DestroyComponent();
				}
			}
			BlockBodies.Empty();
		}

		// Damping: wheels own ground resistance. In FLIGHT the atmosphere
		// (and not a 0.05 angular damper) has to kill roll wobble.
		const bool bWheeled = HasCompleteWheel();
		const bool bFlying = ControlMode == EPilotControlMode::Flight
			&& PilotPawn != nullptr && HasCompleteThruster();
		if (bFlying)
		{
			PhysicsRoot->SetLinearDamping(LinearDampingFlying);
			PhysicsRoot->SetAngularDamping(AngularDampingFlying);
		}
		else
		{
			PhysicsRoot->SetLinearDamping(bWheeled ? LinearDampingWheeled : LinearDampingNoWheels);
			PhysicsRoot->SetAngularDamping(bWheeled ? AngularDampingWheeled : AngularDampingNoWheels);
		}

		// Cargo on the bed is real mass at the construct origin (the chassis).
		const float CargoKg = Cargo ? Cargo->GetCurrentLoad() : 0.f;
		PhysicsRoot->BodyInstance.SetMassOverride(FMath::Max(1.f, 1.f + CargoKg), true);
	}

	// 3. Collision boxes, one per record. Weld state cannot change in place,
	// so a box whose phase-dependent setup changed is recreated.
	TSet<int32> LiveIds;
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (!Record.Def)
		{
			continue;
		}
		LiveIds.Add(Record.BlockInstanceId);

		const FIntVector AabbCells = GetRotatedAabbCells(Record);
		FVector Extent = FVector(AabbCells) * (CellSize * 0.5f);
		const FVector Center = (FVector(Record.Origin) + FVector(AabbCells) * 0.5f) * CellSize;
		const bool bComplete = Record.Phase == EConstructionPhase::Complete;
		const bool bSolid = bAuthority && bComplete;   // Only the server welds physics bodies.

		// Complete wheels get a hub-sized core box instead of the full cell
		// AABB: the suspension + terramechanics model carries the vehicle at
		// ride height (a full-size rigid box would touch ground first and the
		// soil model would never engage), while the core still hard-stops
		// curbs and full-compression terrain clips, and still welds its mass.
		// 0.35 r, not 0.5 r: at half radius the core touched ground at 17.5 cm
		// sinkage and Chaos contacts fought the soil model (wheel jitter when
		// dug in deep).
		if (bComplete && Record.Def->bIsWheel)
		{
			const FVehicleWheelSpec& Wheel = Record.Def->WheelSpec;
			Extent = FVector(
				0.35f * Wheel.RadiusM * ExoneerUnits::CmPerM,
				0.5f * Wheel.WidthM * ExoneerUnits::CmPerM,
				0.35f * Wheel.RadiusM * ExoneerUnits::CmPerM);
		}

		UBoxComponent* Box = BlockBodies.FindRef(Record.BlockInstanceId);
		if (Box)
		{
			const bool bBoxSolid = Box->GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics;
			const bool bBoxGhost = Box->ComponentTags.Contains(GhostBoxTag);
			if (bBoxSolid != bSolid
				|| bBoxGhost == bComplete
				|| !Box->GetUnscaledBoxExtent().Equals(Extent)
				|| !Box->GetRelativeLocation().Equals(Center))
			{
				Box->DestroyComponent();
				Box = nullptr;
				BlockBodies.Remove(Record.BlockInstanceId);
			}
		}
		if (!Box)
		{
			Box = NewObject<UBoxComponent>(this);
			Box->SetBoxExtent(Extent, false);
			if (bSolid)
			{
				// Server, Complete: full query + welded physics contact.
				Box->SetCollisionProfileName(TEXT("BlockAll"));
				Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				// A hull is not scenery to a camera boom. The pilot's chase
				// camera probes on ECC_Camera from a seat INSIDE this body, so
				// a blocking hull would collapse the boom onto the roof on the
				// first frame. Terrain, slabs and structures still push the
				// view in, which is the behaviour that matters at a slab edge.
				Box->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
				// Override mass before the weld so it contributes correctly.
				Box->BodyInstance.SetMassOverride(FMath::Max(1.f, Record.Def->Mass), true);
			}
			else if (bComplete)
			{
				// Client, Complete: query-only so traces and pawn sweeps agree
				// with the server's welded body.
				Box->SetCollisionProfileName(TEXT("BlockAll"));
				Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
				// Same camera-boom exemption as the server body, so the view
				// behaves identically on a client and on the listen host.
				Box->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
			}
			else
			{
				// Ghost / under construction, IDENTICAL on server and client:
				// aim traces must hit it (weld/build targeting) while pawns
				// walk through. Asymmetric profiles here desync movement and
				// break the listen host's welding.
				Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
				Box->SetCollisionResponseToAllChannels(ECR_Ignore);
				Box->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
				Box->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
				Box->ComponentTags.Add(GhostBoxTag);
			}
			// Place before attaching so the weld computes with the final transform.
			Box->SetRelativeLocation(Center);
			Box->RegisterComponent();
			Box->AttachToComponent(PhysicsRoot, FAttachmentTransformRules(EAttachmentRule::KeepRelative, /*bWeldSimulatedBodies*/ bSolid));
			BlockBodies.Add(Record.BlockInstanceId, Box);
		}
	}
	if (bJustEnabledSim && PhysicsRoot)
	{
		SetActorLocationAndRotation(FreezeLocation, FreezeRotation, false, nullptr, ETeleportType::TeleportPhysics);
		PhysicsRoot->SetPhysicsLinearVelocity(FVector::ZeroVector);
		PhysicsRoot->SetPhysicsAngularVelocityInRadians(FVector::ZeroVector);
		PhysicsRoot->SetEnableGravity(true);
	}
	for (auto It = BlockBodies.CreateIterator(); It; ++It)
	{
		if (!LiveIds.Contains(It->Key))
		{
			if (It->Value)
			{
				It->Value->DestroyComponent();
			}
			It.RemoveCurrent();
		}
	}


	// 3. Visual instances. Complete blocks share one ISMC per mesh (the
	// VisualLayers map); ghost/under-construction blocks get separate tagged
	// layers so Blueprints can assign a ghost material to them.
	TMap<UStaticMesh*, TArray<FTransform>> CompleteInstances;
	TMap<UStaticMesh*, TArray<FTransform>> GhostInstances;
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (!Record.Def)
		{
			continue;
		}
		UStaticMesh* Mesh = Record.Def->Mesh.LoadSynchronous();
		if (!Mesh)
		{
			continue;
		}

		// Placeholder meshes are ~100 cm shapes; cells are 25 cm. Scale each
		// instance so the mesh exactly fills the block's UNROTATED footprint.
		// MeshRelativeTransform maps the MESH onto the block frame (the 90 deg
		// roll that stands the engine cylinder on its side for a wheel), so
		// the cell target must be pulled back through that rotation before it
		// becomes a per-axis scale - otherwise a rolled mesh is stretched on
		// the wrong axes. Composition order: scale, then mesh alignment, then
		// the block's own orientation, so nothing rotates twice.
		const FTransform& MeshXf = Record.Def->MeshRelativeTransform;
		const FVector MeshSize = Mesh->GetBoundingBox().GetSize();
		const FVector TargetSize = FVector(Record.Def->SizeInCells) * CellSize;
		const FVector TargetInMeshFrame = MeshXf.GetRotation().UnrotateVector(TargetSize).GetAbs();
		const FTransform ScaleXf(FQuat::Identity, FVector::ZeroVector, FVector(
			MeshSize.X > 0.f ? TargetInMeshFrame.X / MeshSize.X : 1.f,
			MeshSize.Y > 0.f ? TargetInMeshFrame.Y / MeshSize.Y : 1.f,
			MeshSize.Z > 0.f ? TargetInMeshFrame.Z / MeshSize.Z : 1.f));
		const FTransform LocalTransform = ScaleXf * MeshXf * GetBlockLocalTransform(Record);

		if (Record.Phase == EConstructionPhase::Complete)
		{
			// Complete wheels are excluded: ISMC instances cannot spin or
			// steer (indices are discarded and rebuilds are wholesale), so
			// they get dedicated components below. Ghost wheels still render
			// through the ghost layers.
			if (!Record.Def->bIsWheel)
			{
				CompleteInstances.FindOrAdd(Mesh).Add(LocalTransform);
			}
		}
		else
		{
			GhostInstances.FindOrAdd(Mesh).Add(LocalTransform);
		}
	}

	// Dedicated animated mesh per Complete wheel (BOTH sides - modules exist
	// only on the server, so the construct owns these). Pose is re-applied
	// every frame by UpdateWheelVisuals; creation only sets mesh + attachment.
	{
		TSet<int32> LiveWheelIds;
		for (const FVehicleBlockRecord& Record : BlockList.Blocks)
		{
			if (!Record.Def || !Record.Def->bIsWheel || Record.Phase != EConstructionPhase::Complete)
			{
				continue;
			}
			LiveWheelIds.Add(Record.BlockInstanceId);
			if (!WheelVisuals.Contains(Record.BlockInstanceId))
			{
				UStaticMeshComponent* WheelMesh = NewObject<UStaticMeshComponent>(this);
				WheelMesh->SetStaticMesh(Record.Def->Mesh.LoadSynchronous());
				WheelMesh->SetCollisionProfileName(TEXT("NoCollision"));
				WheelMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				WheelMesh->RegisterComponent();
				WheelMesh->AttachToComponent(PhysicsRoot, FAttachmentTransformRules::KeepRelativeTransform);
				WheelVisuals.Add(Record.BlockInstanceId, WheelMesh);
			}
		}
		for (auto It = WheelVisuals.CreateIterator(); It; ++It)
		{
			if (!LiveWheelIds.Contains(It->Key))
			{
				if (It->Value)
				{
					It->Value->DestroyComponent();
				}
				WheelSpinAngles.Remove(It->Key);
				It.RemoveCurrent();
			}
		}
	}

	const auto CreateLayer = [this](UStaticMesh* Mesh, bool bGhost)
	{
		UInstancedStaticMeshComponent* Layer = NewObject<UInstancedStaticMeshComponent>(this);
		Layer->SetStaticMesh(Mesh);
		Layer->SetCollisionProfileName(TEXT("NoCollision"));
		Layer->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		if (bGhost)
		{
			Layer->ComponentTags.Add(GhostLayerTag);
			// Wireframe until real ghost materials are authored: welded and
			// unwelded blocks must read differently at a glance.
			static UMaterialInterface* GhostMaterial =
				LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineDebugMaterials/WireframeMaterial.WireframeMaterial"));
			if (GhostMaterial)
			{
				for (int32 i = 0; i < FMath::Max(1, Mesh->GetStaticMaterials().Num()); ++i)
				{
					Layer->SetMaterial(i, GhostMaterial);
				}
			}
		}
		Layer->RegisterComponent();
		Layer->AttachToComponent(PhysicsRoot, FAttachmentTransformRules::KeepRelativeTransform);
		return Layer;
	};

	// Complete layers.
	for (TPair<UStaticMesh*, TArray<FTransform>>& Pair : CompleteInstances)
	{
		UInstancedStaticMeshComponent* Layer = VisualLayers.FindRef(Pair.Key);
		if (!Layer)
		{
			Layer = CreateLayer(Pair.Key, false);
			VisualLayers.Add(Pair.Key, Layer);
		}
		Layer->ClearInstances();
		Layer->AddInstances(Pair.Value, /*bShouldReturnIndices*/ false);
	}
	for (auto It = VisualLayers.CreateIterator(); It; ++It)
	{
		if (!CompleteInstances.Contains(It->Key))
		{
			if (It->Value)
			{
				It->Value->DestroyComponent();
			}
			It.RemoveCurrent();
		}
	}

	// Ghost layers, tracked by component tag (the header map only keys by mesh).
	TArray<UInstancedStaticMeshComponent*> TaggedLayers;
	GetComponents<UInstancedStaticMeshComponent>(TaggedLayers);
	TaggedLayers.RemoveAll([](const UInstancedStaticMeshComponent* Layer)
	{
		return !Layer->ComponentTags.Contains(GhostLayerTag);
	});
	for (TPair<UStaticMesh*, TArray<FTransform>>& Pair : GhostInstances)
	{
		UInstancedStaticMeshComponent* Layer = nullptr;
		for (UInstancedStaticMeshComponent* Candidate : TaggedLayers)
		{
			if (Candidate->GetStaticMesh() == Pair.Key)
			{
				Layer = Candidate;
				break;
			}
		}
		if (!Layer)
		{
			Layer = CreateLayer(Pair.Key, true);
		}
		Layer->ClearInstances();
		Layer->AddInstances(Pair.Value, /*bShouldReturnIndices*/ false);
	}
	for (UInstancedStaticMeshComponent* Layer : TaggedLayers)
	{
		if (!GhostInstances.Contains(Layer->GetStaticMesh()))
		{
			Layer->DestroyComponent();
		}
	}

	OnBlocksChanged.Broadcast();
}

// --- Split detection ---

void AVehicleConstruct::RunSplitDetection()
{
	if (!HasAuthority())
	{
		return;
	}
	if (BlockList.Blocks.Num() == 0)
	{
		if (PilotPawn)
		{
			IPilotable::Execute_ExitPilot(this, PilotPawn);
		}
		Destroy();
		return;
	}

	// Occupied cells per record; CellToBlock is fresh after RebuildDerivedState.
	TMap<int32, TArray<FIntVector>> CellsById;
	for (const FVehicleBlockRecord& Record : BlockList.Blocks)
	{
		if (!Record.Def)
		{
			continue;
		}
		TArray<FIntVector>& RecordCells = CellsById.Add(Record.BlockInstanceId);
		ExoneerVehicleOrientation::GetOccupiedCells(Record.Origin, Record.Def->SizeInCells, Record.Orientation, RecordCells);
	}

	static const FIntVector FaceOffsets[6] =
	{
		FIntVector(1, 0, 0), FIntVector(-1, 0, 0),
		FIntVector(0, 1, 0), FIntVector(0, -1, 0),
		FIntVector(0, 0, 1), FIntVector(0, 0, -1)
	};

	TSet<int32> Visited;
	const auto FloodFrom = [this, &CellsById, &Visited](int32 SeedId)
	{
		TArray<int32> Island;
		TArray<int32> Queue;
		Queue.Add(SeedId);
		Visited.Add(SeedId);
		while (Queue.Num() > 0)
		{
			const int32 Id = Queue.Pop(EAllowShrinking::No);
			Island.Add(Id);
			const TArray<FIntVector>* RecordCells = CellsById.Find(Id);
			if (!RecordCells)
			{
				continue;
			}
			for (const FIntVector& Cell : *RecordCells)
			{
				for (const FIntVector& Offset : FaceOffsets)
				{
					const int32* Neighbor = CellToBlock.Find(Cell + Offset);
					if (Neighbor && !Visited.Contains(*Neighbor))
					{
						Visited.Add(*Neighbor);
						Queue.Add(*Neighbor);
					}
				}
			}
		}
		return Island;
	};

	// The active cockpit may just have been removed even when the ship stays
	// fully connected - unseat the pilot BEFORE the early return below.
	if (PilotPawn && FindRecord(ActiveCockpitId) == nullptr)
	{
		IPilotable::Execute_ExitPilot(this, PilotPawn);
	}

	// The island holding the first record stays on this construct.
	FloodFrom(BlockList.Blocks[0].BlockInstanceId);
	if (Visited.Num() >= CellsById.Num())
	{
		return; // Fully connected.
	}

	TArray<TArray<int32>> Islands;
	for (const TPair<int32, TArray<FIntVector>>& Pair : CellsById)
	{
		if (!Visited.Contains(Pair.Key))
		{
			Islands.Add(FloodFrom(Pair.Key));
		}
	}

	TSet<int32> IdsToRemove;
	for (const TArray<int32>& Island : Islands)
	{
		bool bHasModuleBlock = false;
		for (const int32 Id : Island)
		{
			const FVehicleBlockRecord* Record = FindRecord(Id);
			if (Record && Record->Def && Record->Def->ModuleClass)
			{
				bHasModuleBlock = true;
				break;
			}
		}

		if (Island.Num() <= ScrapInsteadOfSplitMaxBlocks && !bHasModuleBlock)
		{
			// Too small to bother splitting; scrap drops are a later feature.
			UE_LOG(LogTemp, Log, TEXT("%s: scrapping detached %d-block island."), *GetName(), Island.Num());
			for (const int32 Id : Island)
			{
				IdsToRemove.Add(Id);
				if (UVehicleModule* Module = Modules.FindRef(Id))
				{
					Module->Shutdown();
				}
				Modules.Remove(Id);
				RemoveWheelStateItem(Id);
			}
			continue;
		}

		// Spawn a sibling construct in the same local frame and move the records.
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AVehicleConstruct* NewConstruct = GetWorld()->SpawnActor<AVehicleConstruct>(GetClass(), GetActorTransform(), Params);
		if (!NewConstruct)
		{
			continue;
		}
		int32 MaxMovedId = INDEX_NONE;
		for (const int32 Id : Island)
		{
			const FVehicleBlockRecord* Record = FindRecord(Id);
			if (!Record)
			{
				continue;
			}
			FVehicleBlockRecord& Moved = NewConstruct->BlockList.Blocks.Add_GetRef(*Record);
			Moved.ReplicationID = INDEX_NONE; // Fresh identity in the new fast array.
			Moved.ReplicationKey = INDEX_NONE;
			NewConstruct->BlockList.MarkItemDirty(Moved);
			MaxMovedId = FMath::Max(MaxMovedId, Id);
			IdsToRemove.Add(Id);
			if (UVehicleModule* Module = Modules.FindRef(Id))
			{
				Module->Shutdown();
			}
			Modules.Remove(Id);

			// Wheel side-array state moves with its record; the new construct's
			// module recreates live physics state but persistent settings (tire
			// pressure, steer pose) carry over through the moved item.
			if (const FVehicleWheelStateItem* WheelItem = WheelStates.FindByBlockId(Id))
			{
				FVehicleWheelStateItem& MovedItem = NewConstruct->WheelStates.Items.Add_GetRef(*WheelItem);
				MovedItem.ReplicationID = INDEX_NONE;
				MovedItem.ReplicationKey = INDEX_NONE;
				NewConstruct->WheelStates.MarkItemDirty(MovedItem);
				RemoveWheelStateItem(Id);
			}
		}
		NewConstruct->NextBlockInstanceId = MaxMovedId + 1;
		NewConstruct->RebuildDerivedState();
	}

	if (IdsToRemove.Num() > 0)
	{
		BlockList.Blocks.RemoveAll([&IdsToRemove](const FVehicleBlockRecord& R)
		{
			return IdsToRemove.Contains(R.BlockInstanceId);
		});
		BlockList.MarkArrayDirty();
		RebuildDerivedState();
	}

	// The pilot may have lost its seat in the shuffle.
	if (PilotPawn && FindRecord(ActiveCockpitId) == nullptr)
	{
		IPilotable::Execute_ExitPilot(this, PilotPawn);
	}
	if (BlockList.Blocks.Num() == 0)
	{
		Destroy();
	}
}
