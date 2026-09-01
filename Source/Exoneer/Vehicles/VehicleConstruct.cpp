// Copyright Exoneer contributors.
#include "Vehicles/VehicleConstruct.h"
#include "Exoneer.h"
#include "Vehicles/VehicleModule.h"
#include "Vehicles/VehicleOrientation.h"
#include "Data/VehicleBlockDefinitionDataAsset.h"
#include "Data/ItemDefinitionDataAsset.h"
#include "Components/InventoryComponent.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "ExoneerGameplayTags.h"
#include "Player/PlayerSurvivalCharacter.h"
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

	/** Block transform in construct-local space: orientation quat at the AABB center. */
	FTransform GetBlockLocalTransform(const FVehicleBlockRecord& Record)
	{
		const FIntVector AabbCells = GetRotatedAabbCells(Record);
		const FVector Center = (FVector(Record.Origin) + FVector(AabbCells) * 0.5f) * AVehicleConstruct::CellSize;
		return FTransform(ExoneerVehicleOrientation::GetQuat(Record.Orientation), Center);
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

void FVehicleBlockRecord::PostReplicatedAdd(const FVehicleBlockList& InArray)
{
	if (InArray.OwnerConstruct)
	{
		InArray.OwnerConstruct->MarkVisualsDirty();
	}
}

void FVehicleBlockRecord::PostReplicatedChange(const FVehicleBlockList& InArray)
{
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
		// across the plain forever like it is in orbit.
		PhysicsRoot->SetLinearDamping(0.4f);
		PhysicsRoot->SetAngularDamping(2.f);
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
	if (UWorld* World = GetWorld())
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

float AVehicleConstruct::RepairBlockAt(const FVector& WorldPoint, float Amount)
{
	if (!HasAuthority() || Amount <= 0.f)
	{
		return 0.f;
	}
	FVehicleBlockRecord* Record = FindMutableRecord(FindBlockAtWorldPoint(WorldPoint));
	if (!Record || !Record->Def)
	{
		return 0.f;
	}
	const float Old = Record->Health;
	Record->Health = FMath::Min(Record->Health + Amount, Record->Def->MaxHealth);
	const float Healed = Record->Health - Old;
	if (Healed > 0.f)
	{
		MarkRecordDirty(*Record);
	}
	return Healed;
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

float AVehicleConstruct::GetConstructionProgressAt_Implementation(const FVector& WorldPoint) const
{
	const FVehicleBlockRecord* Record = FindRecord(FindBlockAtWorldPoint(WorldPoint));
	if (!Record || !Record->Def || Record->Phase == EConstructionPhase::Complete)
	{
		return 1.f;
	}
	const TArray<FConstructionCost>& Stages = Record->Def->Stages;
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
		if (i < Record->StageIndex)
		{
			DoneWork += Work;
		}
		else if (i == Record->StageIndex)
		{
			DoneWork += FMath::Clamp(Record->StageProgress01, 0.f, 1.f) * Work;
		}
	}
	return TotalWork > 0.f ? DoneWork / TotalWork : 0.f;
}

float AVehicleConstruct::InvestConstruction_Implementation(AActor* Builder, UInventoryComponent* SourceInventory, const FVector& WorldPoint, float WeldPoints)
{
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
		// UConstructionComponent, spec section 7).
		float ReachableProgress = TargetProgress;
		for (const FInventoryEntry& Material : Stage.Materials)
		{
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
			if (RefundUnits > 0 && RefundInventory)
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
			ControlMode = ControlMode == EPilotControlMode::Flight
				? EPilotControlMode::Ground
				: EPilotControlMode::Flight;
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
			const float ClearHeight = AsCharacter->GetCapsuleComponent()
				? AsCharacter->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + CellSize
				: 100.f;
			AsCharacter->SetActorLocation(
				AsCharacter->GetActorLocation() + GetActorQuat().GetAxisZ() * ClearHeight,
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
		ServerTickPowerLedger(DeltaSeconds);
		ServerRouteThrust(DeltaSeconds);
		ServerTickModules(DeltaSeconds);

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
	}
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

	// Replicate battery charge in 1% steps to keep the fast array quiet.
	const auto SetBatteryCharge = [this](FVehicleBlockRecord& Record, float NewCharge)
	{
		NewCharge = FMath::Clamp(NewCharge, 0.f, 1.f);
		const float OldCharge = Record.StateScalar;
		Record.StateScalar = NewCharge;
		if (FMath::FloorToInt(OldCharge * 100.f) != FMath::FloorToInt(NewCharge * 100.f))
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
				StoredEnergy += Record.StateScalar * Record.Def->EnergyStorage;
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
			const float Capacity = Record.Def->EnergyStorage;
			const float Room = (1.f - Record.StateScalar) * Capacity;
			const float Added = FMath::Min(Room, SurplusEnergy);
			if (Added > 0.f)
			{
				SetBatteryCharge(Record, Record.StateScalar + Added / Capacity);
				SurplusEnergy -= Added;
			}
		}
	}
	PowerSupplyFraction = NewFraction;
}

void AVehicleConstruct::ServerRouteThrust(float DeltaSeconds)
{
	if (!PhysicsRoot || !PhysicsRoot->IsSimulatingPhysics())
	{
		return;
	}

	// The pilot's move intent is expressed in the cockpit's frame.
	const FVehicleBlockRecord* Cockpit = FindRecord(ActiveCockpitId);
	FQuat CockpitQuat = GetActorQuat();
	FVector DesiredWorld = FVector::ZeroVector;
	if (PilotPawn && Cockpit)
	{
		CockpitQuat = GetActorQuat() * ExoneerVehicleOrientation::GetQuat(Cockpit->Orientation);
		DesiredWorld = CockpitQuat.RotateVector(PilotInput.Move);
		if (!DesiredWorld.IsNearlyZero())
		{
			DesiredWorld = DesiredWorld.GetSafeNormal();
		}
	}

	// Throttle each thruster by how well its thrust direction serves the intent.
	for (const TPair<int32, TObjectPtr<UVehicleModule>>& Pair : Modules)
	{
		UThrusterModule* Thruster = Cast<UThrusterModule>(Pair.Value.Get());
		if (!Thruster)
		{
			continue;
		}
		float NewThrottle = 0.f;
		if (!DesiredWorld.IsNearlyZero())
		{
			if (const FVehicleBlockRecord* Record = Thruster->FindRecord())
			{
				const FVector ThrustDirection = -GetBlockWorldTransform(*Record).GetUnitAxis(EAxis::X);
				NewThrottle = FMath::Max(0.f, FVector::DotProduct(ThrustDirection, DesiredWorld));
			}
		}
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

	// Cockpit gyro torque, scaled by total rigid body mass. Rotate intent is
	// (pitch, yaw, roll): pitch turns about the cockpit RIGHT axis, yaw about
	// UP, roll about FORWARD - not a raw vector rotation of the intent.
	// Ground mode gates the gyro (default fully off): a rover gets no free
	// mid-air attitude authority; steering comes from the wheels.
	const float GyroFraction = ControlMode == EPilotControlMode::Ground ? GroundModeGyroFraction : 1.f;
	if (PilotPawn && Cockpit && GyroFraction > KINDA_SMALL_NUMBER && !PilotInput.Rotate.IsNearlyZero())
	{
		const FVector Rotate = PilotInput.Rotate.GetClampedToMaxSize(1.f);
		const FVector TorqueWorld =
			CockpitQuat.GetAxisY() * Rotate.X +   // pitch
			CockpitQuat.GetAxisZ() * Rotate.Y +   // yaw
			CockpitQuat.GetAxisX() * Rotate.Z;    // roll
		PhysicsRoot->AddTorqueInRadians(TorqueWorld * RotationTorquePerKg * PhysicsRoot->GetMass() * GyroFraction);
	}
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
		if (bAnyComplete != PhysicsRoot->IsSimulatingPhysics())
		{
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
		const FVector Extent = FVector(AabbCells) * (CellSize * 0.5f);
		const FVector Center = (FVector(Record.Origin) + FVector(AabbCells) * 0.5f) * CellSize;
		const bool bComplete = Record.Phase == EConstructionPhase::Complete;
		const bool bSolid = bAuthority && bComplete;   // Only the server welds physics bodies.

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
				// Override mass before the weld so it contributes correctly.
				Box->BodyInstance.SetMassOverride(FMath::Max(1.f, Record.Def->Mass), true);
			}
			else if (bComplete)
			{
				// Client, Complete: query-only so traces and pawn sweeps agree
				// with the server's welded body.
				Box->SetCollisionProfileName(TEXT("BlockAll"));
				Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
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
		// instance so the mesh exactly fills the block's UNROTATED footprint
		// (FTransform applies scale before rotation, so per-axis is safe).
		FTransform LocalTransform = GetBlockLocalTransform(Record);
		const FVector MeshSize = Mesh->GetBoundingBox().GetSize();
		const FVector TargetSize = FVector(Record.Def->SizeInCells) * CellSize;
		LocalTransform.SetScale3D(FVector(
			MeshSize.X > 0.f ? TargetSize.X / MeshSize.X : 1.f,
			MeshSize.Y > 0.f ? TargetSize.Y / MeshSize.Y : 1.f,
			MeshSize.Z > 0.f ? TargetSize.Z / MeshSize.Z : 1.f));

		if (Record.Phase == EConstructionPhase::Complete)
		{
			CompleteInstances.FindOrAdd(Mesh).Add(LocalTransform);
		}
		else
		{
			GhostInstances.FindOrAdd(Mesh).Add(LocalTransform);
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
				Modules.Remove(Id);
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
			Modules.Remove(Id);
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
