// Copyright Exoneer contributors.
#include "Components/BuildToolComponent.h"
#include "Components/InventoryComponent.h"
#include "Components/SurvivalStatsComponent.h"
#include "Data/PieceDefinitionDataAsset.h"
#include "Data/VehicleBlockDefinitionDataAsset.h"
#include "Building/BasePiece.h"
#include "Building/BaseStructure.h"
#include "Vehicles/VehicleConstruct.h"
#include "Vehicles/VehicleOrientation.h"
#include "Interfaces/Constructible.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/Pawn.h"
#include "DrawDebugHelpers.h"
#include "Exoneer.h"
#include "ExoneerGameplayTags.h"

namespace
{
	/** Weld intents are batched and flushed to the server at roughly 5 Hz. */
	constexpr float WeldFlushInterval = 0.2f;

	/** The ghost mesh when the definition has one, else the final mesh. */
	UStaticMesh* ResolvePreviewMeshAsset(const UPieceDefinitionDataAsset* Piece, const UVehicleBlockDefinitionDataAsset* Block)
	{
		if (Piece)
		{
			if (UStaticMesh* Ghost = Piece->GhostMesh.LoadSynchronous())
			{
				return Ghost;
			}
			return Piece->Mesh.LoadSynchronous();
		}
		return Block ? Block->Mesh.LoadSynchronous() : nullptr;
	}

	/**
	 * Client-side socket occupancy mirror. The server's OccupiedSockets map
	 * never replicates; instead every piece replicates its ParentPiece and
	 * ParentSocket, so scanning the structure's replicated registry tells us
	 * whether some piece already snapped into this socket. Surface sockets
	 * never fill. The server remains the placement authority on commit.
	 */
	bool IsSocketFreeForPreview(const ABaseStructure* Structure, const ABasePiece* Parent, const FPieceSocketDef& SocketDef)
	{
		if (SocketDef.bSurfaceSocket || !Structure)
		{
			return true;
		}
		for (const TObjectPtr<ABasePiece>& Piece : Structure->Pieces)
		{
			if (IsValid(Piece) && Piece->ParentPiece.Get() == Parent && Piece->ParentSocket == SocketDef.SocketName)
			{
				return false;
			}
		}
		return true;
	}

	bool IsTerrainPlaceable(const UPieceDefinitionDataAsset* Def)
	{
		return Def && (Def->bGroundable || Def->MountTag == ExoneerTags::Mount_Deployable);
	}

	/** Upright placement transform at the hit point, yawed to the view. */
	FTransform MakeGroundedTransform(const AActor* Viewer, const FHitResult& Hit, int32 YawSteps)
	{
		FVector ViewLoc;
		FRotator ViewRot;
		Viewer->GetActorEyesViewPoint(ViewLoc, ViewRot);
		const float Yaw = ViewRot.Yaw + static_cast<float>(YawSteps) * 90.f;
		return FTransform(FRotator(0.f, Yaw, 0.f), Hit.ImpactPoint);
	}
}

UBuildToolComponent::UBuildToolComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// Required so the Server RPCs below can route through the owning pawn.
	SetIsReplicatedByDefault(true);
}

// --- Mode & selection ---

void UBuildToolComponent::SetBuildModeEnabled(bool bEnabled)
{
	if (!bEnabled)
	{
		Mode = EBuildToolMode::None;
		ClearPreview();
		return;
	}
	Mode = (SelectedBlock && !SelectedPiece) ? EBuildToolMode::VehiclePlacement : EBuildToolMode::BasePlacement;
}

namespace
{
	struct FCuratedOrientation
	{
		uint8 Index = 0;
		const TCHAR* Label = TEXT("");
	};

	/**
	 * Thrusters push along block local -X: six aim directions, UP first (hover
	 * thrust is the common case), and each aim offered TWICE because the NOZZLE
	 * IS CANTED and the lean is a placement decision, not a cosmetic one.
	 *
	 * THIS IS THE LIST THAT DECIDES WHETHER A PLAYER-BUILT CRAFT CAN FLY. The
	 * jet leaves the block a few degrees off its own -X face
	 * (UVehicleBlockDefinitionDataAsset::NozzleCantDeg), so four of the 24
	 * orientations aim a unit the same way and differ only in which way the jet
	 * leans. That lean is where a craft made of parallel lift nozzles gets ALL
	 * of its yaw authority: toed outboard on each rail, uniform throttle makes
	 * no yaw at all while a diagonal trim across the four corner units is a
	 * pure yaw couple with no net force.
	 *
	 * The previous list offered one entry per aim, built from the ONE-AXIS
	 * FindOrientationMappingAxis, so every unit a player aimed "up" got the
	 * same orientation index and therefore the same lean. Six of them at the
	 * hover collective are 6 * 4000 * sin(6 deg) * 0.763 = 1904 N of net side
	 * thrust - 1.03 m/s^2 of permanent, uncancellable drift, because the trim
	 * path nulls only the net TRIM force and flight has no lateral thrust
	 * command at all. The one thing a player could not build was the balanced
	 * layout the shipped rover uses, because only the spawner could reach the
	 * two-axis helper.
	 *
	 * TWELVE ENTRIES, NOT TWENTY-FOUR. The lean axis is fixed per aim by one
	 * rule: the first construct axis perpendicular to the aim, taken in the
	 * order Y, Z. Vertical and fore/aft aims therefore lean LEFT or RIGHT (the
	 * pair that toes a rail and makes yaw), and sideways aims lean UP or DOWN.
	 * The two remaining rolls of each aim are the same jet mirrored about an
	 * axis the craft does not care about, so nothing flyable is lost.
	 */
	const TArray<FCuratedOrientation>& GetThrustOrientations()
	{
		static const TArray<FCuratedOrientation> Set = []
		{
			const FVector& ThrustAxis = ExoneerThruster::LocalThrustAxis;
			// The block's own local +Y is the direction the jet leans, so
			// pinning it onto a construct axis pins the toe.
			const FVector& CantAxis = FVector::YAxisVector;
			auto Aim = [&ThrustAxis, &CantAxis](const FVector& Target, const FVector& Lean)
			{
				return ExoneerVehicleOrientation::FindOrientationMappingAxes(
					ThrustAxis, Target, CantAxis, Lean);
			};
			const FVector L = -FVector::YAxisVector;   // toe left
			const FVector R = FVector::YAxisVector;    // toe right
			const FVector U = FVector::ZAxisVector;    // toe up
			const FVector Dn = -FVector::ZAxisVector;  // toe down
			return TArray<FCuratedOrientation>{
				{ Aim(FVector::UpVector, L),        TEXT("THRUST: UP  TOE L") },
				{ Aim(FVector::UpVector, R),        TEXT("THRUST: UP  TOE R") },
				{ Aim(FVector::ForwardVector, L),   TEXT("THRUST: FORWARD  TOE L") },
				{ Aim(FVector::ForwardVector, R),   TEXT("THRUST: FORWARD  TOE R") },
				{ Aim(FVector::BackwardVector, L),  TEXT("THRUST: BACK  TOE L") },
				{ Aim(FVector::BackwardVector, R),  TEXT("THRUST: BACK  TOE R") },
				{ Aim(FVector::LeftVector, U),      TEXT("THRUST: LEFT  TOE UP") },
				{ Aim(FVector::LeftVector, Dn),     TEXT("THRUST: LEFT  TOE DN") },
				{ Aim(FVector::RightVector, U),     TEXT("THRUST: RIGHT  TOE UP") },
				{ Aim(FVector::RightVector, Dn),    TEXT("THRUST: RIGHT  TOE DN") },
				{ Aim(FVector::DownVector, L),      TEXT("THRUST: DOWN  TOE L") },
				{ Aim(FVector::DownVector, R),      TEXT("THRUST: DOWN  TOE R") },
			};
		}();
		return Set;
	}

	/** Everything else on the grid only meaningfully yaws (cockpit facing, wheel axle, symmetric boxes). */
	const TArray<FCuratedOrientation>& GetYawOrientations()
	{
		static const TArray<FCuratedOrientation> Set = []
		{
			return TArray<FCuratedOrientation>{
				{ ExoneerVehicleOrientation::FindOrientationMappingAxis(FVector::ForwardVector, FVector::ForwardVector), TEXT("YAW 0") },
				{ ExoneerVehicleOrientation::FindOrientationMappingAxis(FVector::ForwardVector, FVector::RightVector), TEXT("YAW 90") },
				{ ExoneerVehicleOrientation::FindOrientationMappingAxis(FVector::ForwardVector, FVector::BackwardVector), TEXT("YAW 180") },
				{ ExoneerVehicleOrientation::FindOrientationMappingAxis(FVector::ForwardVector, FVector::LeftVector), TEXT("YAW 270") },
			};
		}();
		return Set;
	}

	const TArray<FCuratedOrientation>& GetOrientationSetFor(const UVehicleBlockDefinitionDataAsset* Block)
	{
		// Ask the question that matters - does this block push along its local
		// -X? A value test on MaxThrust answers a different question and would
		// hijack any future non-thruster block that happens to rate a force.
		const bool bThruster = Block && Block->ModuleClass
			&& Block->ModuleClass->IsChildOf(UThrusterModule::StaticClass());
		return bThruster ? GetThrustOrientations() : GetYawOrientations();
	}
}

void UBuildToolComponent::SetSelectedPiece(UPieceDefinitionDataAsset* Piece)
{
	SelectedPiece = Piece;
	SelectedBlock = nullptr;
	if (Mode != EBuildToolMode::None)
	{
		Mode = EBuildToolMode::BasePlacement;
	}
	OnSelectedBuildableChanged.Broadcast(Piece);
}

void UBuildToolComponent::SetSelectedVehicleBlock(UVehicleBlockDefinitionDataAsset* Block)
{
	SelectedBlock = Block;
	SelectedPiece = nullptr;

	// Snap the persistent orientation into the new block's curated set.
	// Carrying a thruster aim index onto a wheel plants the axle VERTICAL and
	// the label falls through to "ORIENT n" with nothing on screen explaining
	// why the vehicle will not drive.
	if (SelectedBlock)
	{
		const TArray<FCuratedOrientation>& Set = GetOrientationSetFor(SelectedBlock);
		const bool bInSet = Set.ContainsByPredicate(
			[this](const FCuratedOrientation& Entry) { return Entry.Index == Orientation; });
		if (!bInSet && Set.Num() > 0)
		{
			Orientation = Set[0].Index;
		}
	}
	if (Mode != EBuildToolMode::None)
	{
		Mode = EBuildToolMode::VehiclePlacement;
	}
	OnSelectedBuildableChanged.Broadcast(Block);
}

UPrimaryDataAsset* UBuildToolComponent::GetSelected() const
{
	return SelectedPiece ? static_cast<UPrimaryDataAsset*>(SelectedPiece.Get()) : static_cast<UPrimaryDataAsset*>(SelectedBlock.Get());
}

void UBuildToolComponent::CycleOrientation(int32 Steps)
{
	// Vehicle blocks cycle a CURATED aim list (6 thrust directions, or 4
	// yaws) instead of all 24 raw orientations. Base mode keeps the raw
	// counter: it only feeds socket-alternative / ground-yaw modulos.
	if (SelectedBlock)
	{
		const TArray<FCuratedOrientation>& Set = GetOrientationSetFor(SelectedBlock);
		int32 Current = Set.IndexOfByPredicate([this](const FCuratedOrientation& Entry)
		{
			return Entry.Index == Orientation;
		});
		if (Current == INDEX_NONE)
		{
			Current = 0;
		}
		const int32 Next = ((Current + Steps) % Set.Num() + Set.Num()) % Set.Num();
		Orientation = Set[Next].Index;
		return;
	}
	const int32 Wrapped = (static_cast<int32>(Orientation) + Steps) % ExoneerVehicleOrientation::NumOrientations;
	Orientation = static_cast<uint8>(Wrapped < 0 ? Wrapped + ExoneerVehicleOrientation::NumOrientations : Wrapped);
}

FString UBuildToolComponent::GetOrientationLabel() const
{
	if (!SelectedBlock)
	{
		return FString();
	}
	const TArray<FCuratedOrientation>& Set = GetOrientationSetFor(SelectedBlock);
	for (const FCuratedOrientation& Entry : Set)
	{
		if (Entry.Index == Orientation)
		{
			return Entry.Label;
		}
	}
	return FString::Printf(TEXT("ORIENT %d"), Orientation);
}

// --- Tick ---

void UBuildToolComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn)
{
	Super::TickComponent(DeltaTime, TickType, TickFn);

	// Preview and weld intents are strictly local-player work; the server
	// only ever acts through the Server RPCs below.
	const APawn* Pawn = Cast<APawn>(GetOwner());
	if (!Pawn || !Pawn->IsLocallyControlled())
	{
		return;
	}

	if (Mode != EBuildToolMode::None)
	{
		UpdatePreview();
	}
	else if (PreviewMesh && PreviewMesh->IsVisible())
	{
		ClearPreview();
	}

	TickWeldBeam(DeltaTime);
}

// --- Client preview internals ---

void UBuildToolComponent::EnsurePreviewMesh()
{
	if (PreviewMesh || !GetOwner())
	{
		return;
	}
	PreviewMesh = NewObject<UStaticMeshComponent>(GetOwner());
	PreviewMesh->SetMobility(EComponentMobility::Movable);
	PreviewMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PreviewMesh->SetCastShadow(false);
	PreviewMesh->RegisterComponent();
	PreviewMesh->AttachToComponent(GetOwner()->GetRootComponent(),
		FAttachmentTransformRules::KeepWorldTransform);
	PreviewMesh->SetVisibility(false);
}

bool UBuildToolComponent::AimTrace(FHitResult& OutHit) const
{
	const AActor* Owner = GetOwner();
	if (!Owner || !GetWorld())
	{
		return false;
	}

	// The owner's eyes, not "the first UCameraComponent on the actor": a pawn
	// can carry more than one camera (visor plus chase boom) and that lookup
	// returns an arbitrary one. The server reach check uses the same point.
	FVector Start;
	FRotator ViewRot;
	Owner->GetActorEyesViewPoint(Start, ViewRot);
	const FVector End = Start + ViewRot.Vector() * PlacementRange;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ExoneerBuildAim), false, Owner);
	return GetWorld()->LineTraceSingleByChannel(OutHit, Start, End, ECC_Visibility, Params);
}

void UBuildToolComponent::UpdatePreview()
{
	EnsurePreviewMesh();

	// The mode handlers rebuild the candidate from scratch every tick.
	CandidateParent = nullptr;
	CandidateSocket = NAME_None;
	bCandidateGrounded = false;
	CandidateConstruct = nullptr;

	if (PreviewMesh)
	{
		UStaticMesh* Desired = ResolvePreviewMeshAsset(SelectedPiece, SelectedBlock);
		if (PreviewMesh->GetStaticMesh() != Desired)
		{
			PreviewMesh->SetStaticMesh(Desired);
		}
	}

	FHitResult Hit;
	if (!AimTrace(Hit))
	{
		Hit = FHitResult();
	}

	if (Mode == EBuildToolMode::BasePlacement)
	{
		UpdateBasePreview(Hit);
	}
	else if (Mode == EBuildToolMode::VehiclePlacement)
	{
		UpdateVehiclePreview(Hit);
	}
}

void UBuildToolComponent::UpdateBasePreview(const FHitResult& Hit)
{
	if (!SelectedPiece)
	{
		SetPreviewState(false, EBuildPlacementError::NoTarget, FTransform::Identity);
		return;
	}

	// Aiming at an existing piece: snap to its best compatible free socket.
	if (ABasePiece* HitPiece = Cast<ABasePiece>(Hit.GetActor()))
	{
		if (!HitPiece->Def || !HitPiece->OwningStructure)
		{
			SetPreviewState(false, EBuildPlacementError::NoTarget, FTransform::Identity);
			return;
		}

		struct FSocketCandidate
		{
			FName Name;
			FTransform World;
			float DistSq;
		};
		TArray<FSocketCandidate> Candidates;
		bool bAnyCompatibleMount = false;

		for (const FPieceSocketDef& SocketDef : HitPiece->Def->Sockets)
		{
			if (!SocketDef.AcceptedMounts.HasTag(SelectedPiece->MountTag))
			{
				continue;
			}
			bAnyCompatibleMount = true;
			if (!IsSocketFreeForPreview(HitPiece->OwningStructure, HitPiece, SocketDef))
			{
				continue;
			}
			const FTransform World = HitPiece->GetSocketWorldTransform(SocketDef.SocketName);
			Candidates.Add({ SocketDef.SocketName, World,
				static_cast<float>(FVector::DistSquared(World.GetLocation(), Hit.ImpactPoint)) });
		}

		if (Candidates.Num() > 0)
		{
			// Nearest socket wins; the R key cycles through the alternatives.
			Candidates.Sort([](const FSocketCandidate& A, const FSocketCandidate& B) { return A.DistSq < B.DistSq; });
			const FSocketCandidate& Pick = Candidates[static_cast<int32>(Orientation) % Candidates.Num()];
			CandidateParent = HitPiece;
			CandidateSocket = Pick.Name;
			SetPreviewState(true, EBuildPlacementError::None, Pick.World);
			return;
		}

		SetPreviewState(false,
			bAnyCompatibleMount ? EBuildPlacementError::SocketOccupied : EBuildPlacementError::IncompatibleMount,
			FTransform::Identity);
		return;
	}

	// Aiming at terrain (or anything else): groundable pieces may snap there.
	if (Hit.bBlockingHit && !Cast<AVehicleConstruct>(Hit.GetActor()))
	{
		if (!IsTerrainPlaceable(SelectedPiece))
		{
			SetPreviewState(false, EBuildPlacementError::NoTarget, FTransform::Identity);
			return;
		}

		const FTransform GroundXf = MakeGroundedTransform(GetOwner(), Hit, static_cast<int32>(Orientation) % 4);
		const float MinNormalZ = FMath::Cos(FMath::DegreesToRadians(TerrainSlopeLimitDeg));
		if (Hit.ImpactNormal.Z < MinNormalZ)
		{
			SetPreviewState(false, EBuildPlacementError::NoSupport, GroundXf);
			return;
		}

		bCandidateGrounded = true;
		CandidateGroundTransform = GroundXf;
		SetPreviewState(true, EBuildPlacementError::None, GroundXf);
		return;
	}

	SetPreviewState(false, EBuildPlacementError::NoTarget, FTransform::Identity);
}

void UBuildToolComponent::UpdateVehiclePreview(const FHitResult& Hit)
{
	if (!SelectedBlock)
	{
		SetPreviewState(false, EBuildPlacementError::NoTarget, FTransform::Identity);
		return;
	}

	// Aiming at a construct: the candidate is the free cell on the hit face.
	if (AVehicleConstruct* Construct = Cast<AVehicleConstruct>(Hit.GetActor()))
	{
		// One cell step along the hit face normal, in construct grid space.
		const FVector LocalNormal = Construct->GetActorTransform().InverseTransformVectorNoScale(Hit.ImpactNormal);
		FIntVector Step(0, 0, 0);
		if (FMath::Abs(LocalNormal.X) >= FMath::Abs(LocalNormal.Y) && FMath::Abs(LocalNormal.X) >= FMath::Abs(LocalNormal.Z))
		{
			Step.X = LocalNormal.X >= 0.f ? 1 : -1;
		}
		else if (FMath::Abs(LocalNormal.Y) >= FMath::Abs(LocalNormal.Z))
		{
			Step.Y = LocalNormal.Y >= 0.f ? 1 : -1;
		}
		else
		{
			Step.Z = LocalNormal.Z >= 0.f ? 1 : -1;
		}

		// Sample slightly inside the face so we land in the hit block's cell.
		const FIntVector HitCell = Construct->WorldToCell(Hit.ImpactPoint - Hit.ImpactNormal * (AVehicleConstruct::CellSize * 0.25f));
		const FIntVector Origin = HitCell + Step;

		FVehicleBlockRecord PreviewRecord;
		PreviewRecord.Def = SelectedBlock;
		PreviewRecord.Origin = Origin;
		PreviewRecord.Orientation = Orientation;
		const FTransform Where = Construct->GetBlockWorldTransform(PreviewRecord);

		// Client mirror of CanPlaceBlock: all cells free, one occupied neighbor.
		TArray<FIntVector> Cells;
		ExoneerVehicleOrientation::GetOccupiedCells(Origin, SelectedBlock->SizeInCells, Orientation, Cells);
		static const FIntVector Neighbors[6] =
		{
			FIntVector(1, 0, 0), FIntVector(-1, 0, 0),
			FIntVector(0, 1, 0), FIntVector(0, -1, 0),
			FIntVector(0, 0, 1), FIntVector(0, 0, -1)
		};
		bool bAllFree = true;
		bool bAdjacent = false;
		for (const FIntVector& Cell : Cells)
		{
			if (Construct->FindBlockAtCell(Cell) != INDEX_NONE)
			{
				bAllFree = false;
				break;
			}
			for (const FIntVector& Neighbor : Neighbors)
			{
				if (Construct->FindBlockAtCell(Cell + Neighbor) != INDEX_NONE)
				{
					bAdjacent = true;
					break;
				}
			}
		}

		if (!bAllFree)
		{
			SetPreviewState(false, EBuildPlacementError::CellOccupied, Where);
			return;
		}
		if (!bAdjacent)
		{
			SetPreviewState(false, EBuildPlacementError::NotAdjacent, Where);
			return;
		}

		CandidateConstruct = Construct;
		CandidateCell = Origin;
		SetPreviewState(true, EBuildPlacementError::None, Where);
		return;
	}

	// No construct under the cursor: structural frames may found a new one.
	if (Hit.bBlockingHit && !Cast<ABasePiece>(Hit.GetActor()))
	{
		if (SelectedBlock->ModuleClass)
		{
			SetPreviewState(false, EBuildPlacementError::NoTarget, FTransform::Identity);
			return;
		}
		const FTransform GroundXf = MakeGroundedTransform(GetOwner(), Hit, static_cast<int32>(Orientation) % 4);
		bCandidateGrounded = true;
		CandidateGroundTransform = GroundXf;

		// The construct origin lands on the floor; its first block occupies the
		// cells ABOVE it, so the ghost renders at the future block center.
		FTransform PreviewXf = GroundXf;
		PreviewXf.AddToTranslation(FVector(0.f, 0.f,
			static_cast<float>(SelectedBlock->SizeInCells.Z) * AVehicleConstruct::CellSize * 0.5f));
		SetPreviewState(true, EBuildPlacementError::None, PreviewXf);
		return;
	}

	SetPreviewState(false, EBuildPlacementError::NoTarget, FTransform::Identity);
}

void UBuildToolComponent::SetPreviewState(bool bValid, EBuildPlacementError Error, const FTransform& Where)
{
	// An identity transform means "nothing to show" (error-only state).
	if (PreviewMesh)
	{
		const bool bShow = Mode != EBuildToolMode::None && !Where.Equals(FTransform::Identity);
		PreviewMesh->SetVisibility(bShow);
		if (bShow)
		{
			// Match the placed result: base pieces render bottom-aligned at the
			// mount point; vehicle blocks scale the ~100 cm placeholder mesh
			// down to the block's cell footprint.
			FTransform Final = Where;
			if (UStaticMesh* SM = PreviewMesh->GetStaticMesh())
			{
				const FBox Bounds = SM->GetBoundingBox();
				if (Mode == EBuildToolMode::BasePlacement)
				{
					Final.AddToTranslation(Final.GetRotation().RotateVector(FVector(0.f, 0.f, -Bounds.Min.Z)));
				}
				else if (SelectedBlock)
				{
					const FVector MeshSize = Bounds.GetSize();
					const FVector TargetSize = FVector(SelectedBlock->SizeInCells) * AVehicleConstruct::CellSize;
					Final.SetScale3D(FVector(
						MeshSize.X > 0.f ? TargetSize.X / MeshSize.X : 1.f,
						MeshSize.Y > 0.f ? TargetSize.Y / MeshSize.Y : 1.f,
						MeshSize.Z > 0.f ? TargetSize.Z / MeshSize.Z : 1.f));
				}
			}
			PreviewMesh->SetWorldTransform(Final);
			UMaterialInterface* Mat = (bValid ? ValidPreviewMaterial : InvalidPreviewMaterial).LoadSynchronous();
			if (Mat)
			{
				for (int32 i = 0; i < PreviewMesh->GetNumMaterials(); ++i)
				{
					PreviewMesh->SetMaterial(i, Mat);
				}
			}
		}
	}

	// The HUD only hears about state changes, not every preview tick.
	if (bValid != bLastPreviewValid || Error != LastError)
	{
		bLastPreviewValid = bValid;
		LastError = Error;
		OnBuildPreviewChanged.Broadcast(bValid, Error);
	}
}

void UBuildToolComponent::ClearPreview()
{
	CandidateParent = nullptr;
	CandidateSocket = NAME_None;
	bCandidateGrounded = false;
	CandidateConstruct = nullptr;
	if (PreviewMesh)
	{
		PreviewMesh->SetVisibility(false);
	}
	if (bLastPreviewValid || LastError != EBuildPlacementError::None)
	{
		bLastPreviewValid = false;
		LastError = EBuildPlacementError::None;
		OnBuildPreviewChanged.Broadcast(false, EBuildPlacementError::None);
	}
}

// --- Actions ---

bool UBuildToolComponent::TryConfirmPlacement()
{
	if (Mode == EBuildToolMode::None || !bLastPreviewValid)
	{
		return false;
	}

	if (Mode == EBuildToolMode::BasePlacement && SelectedPiece)
	{
		if (CandidateParent)
		{
			Server_PlaceBasePiece(SelectedPiece, CandidateParent, CandidateSocket);
			return true;
		}
		if (bCandidateGrounded)
		{
			Server_PlaceGroundedPiece(SelectedPiece, CandidateGroundTransform);
			return true;
		}
	}
	else if (Mode == EBuildToolMode::VehiclePlacement && SelectedBlock)
	{
		if (CandidateConstruct)
		{
			Server_PlaceVehicleBlock(CandidateConstruct, SelectedBlock, CandidateCell, Orientation);
			return true;
		}
		if (bCandidateGrounded)
		{
			Server_FoundVehicleConstruct(SelectedBlock, CandidateGroundTransform, Orientation);
			return true;
		}
	}
	return false;
}

void UBuildToolComponent::SetWeldActive(bool bActive)
{
	if (bActive && !bWeldActive)
	{
		// A new press. Its id rides on every batch the press produces, so the
		// server can tell the deliberate press apart from the tail of a hold
		// without guessing from timing or from what the beam happens to touch.
		++WeldPressId;
		bWeldPressBatchPending = true;
	}
	bWeldActive = bActive;
	if (!bWeldActive)
	{
		bWeldPressBatchPending = false;
		bLiveWeldTargetValid = false;
	}
	if (!bWeldActive && !bDeconstructActive)
	{
		WeldRpcAccumulator = 0.f;
	}
}

void UBuildToolComponent::SetDeconstructActive(bool bActive)
{
	bDeconstructActive = bActive;
	if (!bWeldActive && !bDeconstructActive)
	{
		WeldRpcAccumulator = 0.f;
	}
}

bool UBuildToolComponent::WeldAimSweep(FHitResult& OutHit) const
{
	const AActor* Owner = GetOwner();
	if (!Owner || !GetWorld())
	{
		return false;
	}

	// The owner's eyes, not "the first UCameraComponent on the actor": a pawn
	// can carry more than one camera (visor plus chase boom) and that lookup
	// returns an arbitrary one. The server reach check uses the same point.
	FVector Start;
	FRotator ViewRot;
	Owner->GetActorEyesViewPoint(Start, ViewRot);
	const FVector End = Start + ViewRot.Vector() * PlacementRange;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ExoneerWeldAim), false, Owner);
	return GetWorld()->SweepSingleByChannel(OutHit, Start, End, FQuat::Identity,
		ECC_Visibility, FCollisionShape::MakeSphere(WeldAimRadius), Params);
}

void UBuildToolComponent::TickWeldBeam(float DeltaTime)
{
	if (!bWeldActive && !bDeconstructActive)
	{
		WeldRpcAccumulator = 0.f;
		bLiveWeldTargetValid = false;
		LastWeldTargetId = ExoneerConstruction::NoTargetId;
		WeldFeedbackTarget = nullptr;
		return;
	}

	// Bank at most one second of work: a frame hitch must not inflate the next
	// flush past what the server accepts (and the server caps at 1 s anyway).
	WeldRpcAccumulator = FMath::Min(WeldRpcAccumulator + WeldPointsPerSec * DeltaTime, WeldPointsPerSec);

	// A generous sphere sweep, every tick: it feeds both the aim dot and the
	// flush below. Green dot = constructible under the beam.
	FHitResult Hit;
	const bool bHit = WeldAimSweep(Hit);
	AActor* Target = bHit ? Hit.GetActor() : nullptr;
	const bool bConstructible = Target && Target->Implements<UConstructible>();
#if ENABLE_DRAW_DEBUG
	if (bHit)
	{
		DrawDebugSphere(GetWorld(), Hit.ImpactPoint, 6.f, 8,
			bConstructible ? FColor::Green : FColor::White, false, 0.05f);
	}
#endif

	// Build progress at frame rate, of the block that ACTUALLY took the work.
	// The 5 Hz server feedback still says WHY a weld stalls, and it also names
	// the target; the percentage itself is read here off the replicated
	// construction state, so the readout counts every value on the way to 100
	// instead of stepping once per flush.
	//
	// Sampling by aim point instead is what made this read 100 percent early:
	// the sweep is 14 cm and a block is 25 cm, so while welding a ghost beside
	// a finished block the point resolves to the FINISHED one, and a
	// point-addressed progress query answers 1.0 for it - and for a clean miss
	// too. Identity removes both cases.
	AActor* FeedbackTarget = WeldFeedbackTarget.Get();
	if (bConstructible && FeedbackTarget == Target && LastWeldTargetId != ExoneerConstruction::NoTargetId)
	{
		const float Progress01 = IConstructible::Execute_GetConstructionProgressForTarget(Target, LastWeldTargetId);
		bLiveWeldTargetValid = Progress01 >= 0.f;
		if (bLiveWeldTargetValid)
		{
			LiveWeldProgress01 = FMath::Clamp(Progress01, 0.f, 1.f);
		}
	}
	else
	{
		bLiveWeldTargetValid = false;
	}

	// The first batch of a press flushes as soon as the beam finds something:
	// a deliberate press must not have to be held for a flush interval before
	// the server hears about it, and progress starts moving on contact.
	const float FlushPoints = WeldPointsPerSec * WeldFlushInterval;
	const bool bFirstBatchOfPress = bWeldActive && bWeldPressBatchPending;
	if (WeldRpcAccumulator < FlushPoints && !(bFirstBatchOfPress && bConstructible))
	{
		return;
	}

	// No target this instant: KEEP the banked points (capped above) so a brief
	// aim slip does not eat progress, and surface why nothing happens.
	if (!bConstructible)
	{
		LastWeldResult = 4;
		LastWeldProgress01 = 0.f;
		LastWeldTargetId = ExoneerConstruction::NoTargetId;
		WeldFeedbackTarget = nullptr;
		LastWeldFeedbackSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
		return;
	}

	const float Points = WeldRpcAccumulator;
	WeldRpcAccumulator = 0.f;

	if (bWeldActive)
	{
		bWeldPressBatchPending = false;
		WeldFeedbackTarget = Target;
		Server_Weld(Target, Hit.ImpactPoint, Points, WeldPressId);
	}
	else
	{
		Server_Deconstruct(Target, Hit.ImpactPoint, Points);
	}
}

void UBuildToolComponent::Client_WeldFeedback_Implementation(uint8 Result, float Progress01, int32 TargetId)
{
	// The visor HUD polls these; no direct drawing from the component.
	LastWeldResult = Result;
	LastWeldProgress01 = Progress01;
	LastWeldTargetId = TargetId;
	LastWeldFeedbackSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
}

// --- Server helpers ---

bool UBuildToolComponent::ServerValidateReach(const FVector& Point) const
{
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return false;
	}
	FVector ViewLoc;
	FRotator ViewRot;
	Owner->GetActorEyesViewPoint(ViewLoc, ViewRot);
	return FVector::DistSquared(ViewLoc, Point) <= FMath::Square(PlacementRange * 1.5f);
}

bool UBuildToolComponent::ServerValidateGrounded(const FTransform& Transform) const
{
	// The client authored this transform; re-derive the ground truth here:
	// upright (yaw-only), actual terrain under the point, slope within limit.
	if (!GetWorld())
	{
		return false;
	}
	if (!Transform.GetRotation().GetAxisZ().Equals(FVector::UpVector, 0.05f))
	{
		return false;
	}

	const FVector Location = Transform.GetLocation();
	FHitResult Ground;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ExoneerGroundCheck), false, GetOwner());
	if (!GetWorld()->LineTraceSingleByChannel(Ground,
		Location + FVector(0.f, 0.f, 50.f), Location - FVector(0.f, 0.f, 150.f),
		ECC_Visibility, Params))
	{
		return false;
	}
	if (FVector::DistSquared(Ground.ImpactPoint, Location) > FMath::Square(100.f))
	{
		return false;
	}
	const float MinNormalZ = FMath::Cos(FMath::DegreesToRadians(TerrainSlopeLimitDeg));
	return Ground.ImpactNormal.Z >= MinNormalZ;
}

void UBuildToolComponent::Client_PlacementRejected_Implementation(EBuildPlacementError Error)
{
	// Surface the server's verdict on the HUD; the next preview tick resets it.
	bLastPreviewValid = false;
	LastError = Error;
	OnBuildPreviewChanged.Broadcast(false, Error);
}

UInventoryComponent* UBuildToolComponent::GetOwnerInventory() const
{
	return GetOwner() ? GetOwner()->FindComponentByClass<UInventoryComponent>() : nullptr;
}

// --- Commit RPCs ---

bool UBuildToolComponent::Server_PlaceBasePiece_Validate(UPieceDefinitionDataAsset* Def, ABasePiece* Parent, FName Socket)
{
	return Def != nullptr;
}

void UBuildToolComponent::Server_PlaceBasePiece_Implementation(UPieceDefinitionDataAsset* Def, ABasePiece* Parent, FName Socket)
{
	if (!Def || !IsValid(Parent) || !Parent->OwningStructure)
	{
		return;
	}
	if (!ServerValidateReach(Parent->GetSocketWorldTransform(Socket).GetLocation()))
	{
		return;
	}

	ABaseStructure* Structure = Parent->OwningStructure;
	EBuildPlacementError Error = EBuildPlacementError::None;
	if (!Structure->CanPlacePiece(Def, Parent, Socket, Error))
	{
		UE_LOG(LogExoneer, Verbose, TEXT("Server_PlaceBasePiece rejected (%d) for %s"),
			static_cast<int32>(Error), *Def->PieceId.ToString());
		Client_PlacementRejected(Error);
		return;
	}
	Structure->PlacePieceGhost(Def, Parent, Socket);
}

bool UBuildToolComponent::Server_PlaceGroundedPiece_Validate(UPieceDefinitionDataAsset* Def, FTransform Transform)
{
	return Def != nullptr && !Transform.ContainsNaN();
}

void UBuildToolComponent::Server_PlaceGroundedPiece_Implementation(UPieceDefinitionDataAsset* Def, FTransform Transform)
{
	if (!Def || !(Def->bGroundable || Def->MountTag == ExoneerTags::Mount_Deployable))
	{
		return;
	}
	if (!ServerValidateReach(Transform.GetLocation()) || !ServerValidateGrounded(Transform))
	{
		Client_PlacementRejected(EBuildPlacementError::NoSupport);
		return;
	}

	EBuildPlacementError Error = EBuildPlacementError::None;
	if (!ABaseStructure::PlaceGroundedGhost(GetWorld(), Def, Transform, Error))
	{
		UE_LOG(LogExoneer, Verbose, TEXT("Server_PlaceGroundedPiece rejected (%d) for %s"),
			static_cast<int32>(Error), *Def->PieceId.ToString());
		Client_PlacementRejected(Error);
	}
}

bool UBuildToolComponent::Server_PlaceVehicleBlock_Validate(AVehicleConstruct* Construct, UVehicleBlockDefinitionDataAsset* Def, FIntVector Origin, uint8 InOrientation)
{
	return Def != nullptr && InOrientation < ExoneerVehicleOrientation::NumOrientations;
}

void UBuildToolComponent::Server_PlaceVehicleBlock_Implementation(AVehicleConstruct* Construct, UVehicleBlockDefinitionDataAsset* Def, FIntVector Origin, uint8 InOrientation)
{
	if (!IsValid(Construct) || !Def)
	{
		return;
	}
	if (!ServerValidateReach(Construct->CellToWorld(Origin)))
	{
		return;
	}

	EBuildPlacementError Error = EBuildPlacementError::None;
	if (!Construct->CanPlaceBlock(Def, Origin, InOrientation, Error))
	{
		UE_LOG(LogExoneer, Verbose, TEXT("Server_PlaceVehicleBlock rejected (%d) for %s"),
			static_cast<int32>(Error), *Def->BlockId.ToString());
		Client_PlacementRejected(Error);
		return;
	}
	Construct->PlaceBlockGhost(Def, Origin, InOrientation);
}

bool UBuildToolComponent::Server_FoundVehicleConstruct_Validate(UVehicleBlockDefinitionDataAsset* Def, FTransform Transform, uint8 InOrientation)
{
	return Def != nullptr && !Transform.ContainsNaN() && InOrientation < ExoneerVehicleOrientation::NumOrientations;
}

void UBuildToolComponent::Server_FoundVehicleConstruct_Implementation(UVehicleBlockDefinitionDataAsset* Def, FTransform Transform, uint8 InOrientation)
{
	// Only structural frames may found a new construct.
	if (!Def || Def->ModuleClass)
	{
		return;
	}
	if (!ServerValidateReach(Transform.GetLocation()) || !ServerValidateGrounded(Transform))
	{
		Client_PlacementRejected(EBuildPlacementError::NoSupport);
		return;
	}

	EBuildPlacementError Error = EBuildPlacementError::None;
	if (!AVehicleConstruct::FoundConstruct(GetWorld(), Def, Transform, Error, InOrientation))
	{
		UE_LOG(LogExoneer, Verbose, TEXT("Server_FoundVehicleConstruct rejected (%d) for %s"),
			static_cast<int32>(Error), *Def->BlockId.ToString());
		Client_PlacementRejected(Error);
	}
}

bool UBuildToolComponent::Server_Weld_Validate(AActor* Target, FVector_NetQuantize WorldPoint, float WeldPoints, uint8 PressId)
{
	// Generous sanity cap only - a lagged/hitched honest client may batch big;
	// the implementation clamps the applied budget to one second of work.
	// _Validate failure disconnects the client, so never gate on timing here.
	return FMath::IsFinite(WeldPoints) && WeldPoints >= 0.f && WeldPoints <= FMath::Max(WeldPointsPerSec, 1.f) * 60.f;
}

void UBuildToolComponent::Server_Weld_Implementation(AActor* Target, FVector_NetQuantize WorldPoint, float WeldPoints, uint8 PressId)
{
	// A press id the server has not seen is the first batch of a new press;
	// every repeat of the same id is the tail of that hold. The boundary is
	// carried by the batch rather than inferred from timing, in line with the
	// _Validate policy above, and rather than from what the beam resolves to,
	// which changes on any aim wobble.
	const bool bFreshPress = !bServerWeldPressSeen || PressId != ServerWeldPressId;
	ServerWeldPressId = PressId;
	bServerWeldPressSeen = true;

	ServerApplyWeld(Target, WorldPoint, WeldPoints, bFreshPress);
}

void UBuildToolComponent::ServerApplyWeld(AActor* Target, const FVector& WorldPoint, float WeldPoints, bool bFreshPress)
{
	if (!IsValid(Target) || !Target->Implements<UConstructible>() || WeldPoints <= 0.f)
	{
		return;
	}
	if (!ServerValidateReach(WorldPoint))
	{
		return;
	}

	AActor* Owner = GetOwner();
	USurvivalStatsComponent* Stats = Owner ? Owner->FindComponentByClass<USurvivalStatsComponent>() : nullptr;

	// Cap the batch at one second of work, then at what the suit can pay for.
	float Budget = FMath::Min(WeldPoints, WeldPointsPerSec);
	if (Stats)
	{
		if (Stats->SuitPower <= 0.f)
		{
			Client_WeldFeedback(1, 0.f, ExoneerConstruction::NoTargetId);   // Empty suit: the silent killer.
			return;
		}
		if (SuitPowerPerWeldPoint > 0.f)
		{
			Budget = FMath::Min(Budget, Stats->SuitPower / SuitPowerPerWeldPoint);
		}
	}
	if (Budget <= 0.f)
	{
		Client_WeldFeedback(1, 0.f, ExoneerConstruction::NoTargetId);
		return;
	}

	// Investing is always legal, on any batch of any press: welding a ghost is
	// what the beam is for and a hold must keep grinding until the part is
	// Complete.
	// The work reports WHICH target took it. On a moving construct the aim
	// often grazes the finished neighbour of the intended ghost, so the target
	// under WorldPoint and the target that took the weld are not the same
	// thing - echo the one that took it, or the readout jumps to 100 percent
	// before the block is done.
	int32 WeldedTargetId = ExoneerConstruction::NoTargetId;
	const float Applied = IConstructible::Execute_InvestConstruction(Target, Owner, GetOwnerInventory(), WorldPoint, Budget, WeldedTargetId);
	if (Applied > 0.f)
	{
		if (Stats)
		{
			Stats->AddSuitPower(-Applied * SuitPowerPerWeldPoint);
		}
		const float Progress01 = FMath::Clamp(
			IConstructible::Execute_GetConstructionProgressForTarget(Target, WeldedTargetId), 0.f, 1.f);
		Client_WeldFeedback(0, Progress01, WeldedTargetId);
		return;
	}

	// Nothing left to invest here. Everything below either spends a fabricated
	// spare or clears a surface, so it is reachable ONLY from the first batch
	// of a deliberate press. The tail of the hold that just finished a weld
	// falls out here and does nothing - which is the whole point: overholding
	// the beam a moment too long must never touch the part it just built.
	const EConstructionPhase PhaseNow = IConstructible::Execute_GetConstructionPhaseAt(Target, WorldPoint);
	if (!bFreshPress)
	{
		// Still report the honest reason - a hold stalled on a missing material
		// unit must not read as "target complete".
		Client_WeldFeedback(PhaseNow == EConstructionPhase::Complete ? 3 : 2,
			PhaseNow == EConstructionPhase::Complete ? 1.f : 0.f,
			ExoneerConstruction::NoTargetId);
		return;
	}

	// A resolved construction target that reads Complete. Resolve it
	// explicitly: both GetConstructionPhaseAt implementations answer "Complete"
	// for a point that names NO target at all, so the phase alone would route
	// the maintenance verbs off a miss.
	if (AVehicleConstruct* Construct = Cast<AVehicleConstruct>(Target))
	{
		const int32 BlockId = Construct->FindBlockAtWorldPoint(WorldPoint);
		const FVehicleBlockRecord* Record = Construct->FindRecord(BlockId);
		if (!Record || Record->Phase != EConstructionPhase::Complete)
		{
			Client_WeldFeedback(BlockId == INDEX_NONE ? 4 : 2, 0.f, ExoneerConstruction::NoTargetId);
			return;
		}
		if (Construct->WipePartAt(WorldPoint))
		{
			Client_WeldFeedback(7, 1.f, ExoneerConstruction::NoTargetId);
			return;
		}
		if (Construct->ReplacePartAt(WorldPoint, GetOwnerInventory()))
		{
			if (Stats)
			{
				Stats->AddSuitPower(-Budget * SuitPowerPerWeldPoint);
			}
			Client_WeldFeedback(6, 1.f, ExoneerConstruction::NoTargetId);
			return;
		}
		Client_WeldFeedback(3, 1.f, ExoneerConstruction::NoTargetId);
		return;
	}

	if (ABasePiece* Piece = Cast<ABasePiece>(Target))
	{
		if (PhaseNow != EConstructionPhase::Complete)
		{
			Client_WeldFeedback(2, 0.f, ExoneerConstruction::NoTargetId); // missing materials
			return;
		}
		// Terminal reading first: a spent battery pack takes the fabricated
		// spare its definition names. Dust never reaches here - wipe clears
		// dust in full and is the only dust verb - so the two cannot race.
		if (Piece->ReplacePart(GetOwnerInventory()))
		{
			if (Stats)
			{
				Stats->AddSuitPower(-Budget * SuitPowerPerWeldPoint);
			}
			Client_WeldFeedback(6, 1.f, ExoneerConstruction::NoTargetId);
			return;
		}
		if (Piece->WipeDust())
		{
			Client_WeldFeedback(7, 1.f, ExoneerConstruction::NoTargetId);
			return;
		}
		Client_WeldFeedback(3, 1.f, ExoneerConstruction::NoTargetId);
		return;
	}

	Client_WeldFeedback(PhaseNow == EConstructionPhase::Complete ? 3 : 2, 0.f, ExoneerConstruction::NoTargetId);
}

bool UBuildToolComponent::Server_Deconstruct_Validate(AActor* Target, FVector_NetQuantize WorldPoint, float WreckPoints)
{
	// Same policy as Server_Weld_Validate: sanity only, never timing.
	return FMath::IsFinite(WreckPoints) && WreckPoints >= 0.f && WreckPoints <= FMath::Max(WeldPointsPerSec, 1.f) * 60.f;
}

void UBuildToolComponent::Server_Deconstruct_Implementation(AActor* Target, FVector_NetQuantize WorldPoint, float WreckPoints)
{
	if (!IsValid(Target) || !Target->Implements<UConstructible>() || WreckPoints <= 0.f)
	{
		return;
	}
	if (!ServerValidateReach(WorldPoint))
	{
		return;
	}

	AActor* Owner = GetOwner();
	USurvivalStatsComponent* Stats = Owner ? Owner->FindComponentByClass<USurvivalStatsComponent>() : nullptr;

	float Budget = FMath::Min(WreckPoints, WeldPointsPerSec);
	if (Stats)
	{
		if (Stats->SuitPower <= 0.f)
		{
			return;
		}
		if (SuitPowerPerWeldPoint > 0.f)
		{
			Budget = FMath::Min(Budget, Stats->SuitPower / SuitPowerPerWeldPoint);
		}
	}
	if (Budget <= 0.f)
	{
		return;
	}

	const float Applied = IConstructible::Execute_DeconstructAt(Target, Owner, GetOwnerInventory(), WorldPoint, Budget);
	if (Applied > 0.f && Stats)
	{
		Stats->AddSuitPower(-Applied * SuitPowerPerWeldPoint);
	}
}
