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
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/Pawn.h"
#include "DrawDebugHelpers.h"
#include "Exoneer.h"

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
	// Walk through ALL 24 orientations (yaw first, then up-axis via wrap) so
	// thrusters can point in every direction, not just horizontally. Base mode
	// reuses the same counter to cycle socket alternatives / ground yaw.
	const int32 Wrapped = (static_cast<int32>(Orientation) + Steps) % ExoneerVehicleOrientation::NumOrientations;
	Orientation = static_cast<uint8>(Wrapped < 0 ? Wrapped + ExoneerVehicleOrientation::NumOrientations : Wrapped);
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

	FVector Start;
	FRotator ViewRot;
	if (const UCameraComponent* Cam = Owner->FindComponentByClass<UCameraComponent>())
	{
		Start = Cam->GetComponentLocation();
		ViewRot = Cam->GetComponentRotation();
	}
	else
	{
		Owner->GetActorEyesViewPoint(Start, ViewRot);
	}
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
		if (!SelectedPiece->bGroundable)
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
	bWeldActive = bActive;
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

	FVector Start;
	FRotator ViewRot;
	if (const UCameraComponent* Cam = Owner->FindComponentByClass<UCameraComponent>())
	{
		Start = Cam->GetComponentLocation();
		ViewRot = Cam->GetComponentRotation();
	}
	else
	{
		Owner->GetActorEyesViewPoint(Start, ViewRot);
	}
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

	const float FlushPoints = WeldPointsPerSec * WeldFlushInterval;
	if (WeldRpcAccumulator < FlushPoints)
	{
		return;
	}

	// No target this instant: KEEP the banked points (capped above) so a brief
	// aim slip does not eat progress, and surface why nothing happens.
	if (!bConstructible)
	{
		LastWeldResult = 4;
		LastWeldProgress01 = 0.f;
		LastWeldFeedbackSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
		return;
	}

	const float Points = WeldRpcAccumulator;
	WeldRpcAccumulator = 0.f;

	if (bWeldActive)
	{
		Server_Weld(Target, Hit.ImpactPoint, Points);
	}
	else
	{
		Server_Deconstruct(Target, Hit.ImpactPoint, Points);
	}
}

void UBuildToolComponent::Client_WeldFeedback_Implementation(uint8 Result, float Progress01)
{
	// The visor HUD polls these; no direct drawing from the component.
	LastWeldResult = Result;
	LastWeldProgress01 = Progress01;
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
	if (!Def || !Def->bGroundable)
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

bool UBuildToolComponent::Server_Weld_Validate(AActor* Target, FVector_NetQuantize WorldPoint, float WeldPoints)
{
	// Generous sanity cap only - a lagged/hitched honest client may batch big;
	// the implementation clamps the applied budget to one second of work.
	// _Validate failure disconnects the client, so never gate on timing here.
	return FMath::IsFinite(WeldPoints) && WeldPoints >= 0.f && WeldPoints <= FMath::Max(WeldPointsPerSec, 1.f) * 60.f;
}

void UBuildToolComponent::Server_Weld_Implementation(AActor* Target, FVector_NetQuantize WorldPoint, float WeldPoints)
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
			Client_WeldFeedback(1, 0.f);   // Empty suit: the silent killer.
			return;
		}
		if (SuitPowerPerWeldPoint > 0.f)
		{
			Budget = FMath::Min(Budget, Stats->SuitPower / SuitPowerPerWeldPoint);
		}
	}
	if (Budget <= 0.f)
	{
		Client_WeldFeedback(1, 0.f);
		return;
	}

	float Applied = IConstructible::Execute_InvestConstruction(Target, Owner, GetOwnerInventory(), WorldPoint, Budget);
	const EConstructionPhase PhaseNow = IConstructible::Execute_GetConstructionPhaseAt(Target, WorldPoint);

	// Nothing left to invest: welding a Complete, damaged target repairs it.
	if (Applied <= 0.f && PhaseNow == EConstructionPhase::Complete)
	{
		float Healed = 0.f;
		if (ABasePiece* Piece = Cast<ABasePiece>(Target))
		{
			Healed = Piece->RepairHealth(Budget * RepairHealthPerWeldPoint);
		}
		else if (AVehicleConstruct* Construct = Cast<AVehicleConstruct>(Target))
		{
			Healed = Construct->RepairBlockAt(WorldPoint, Budget * RepairHealthPerWeldPoint);
		}
		if (Healed > 0.f && RepairHealthPerWeldPoint > 0.f)
		{
			Applied = Healed / RepairHealthPerWeldPoint;
		}
	}

	if (Applied > 0.f)
	{
		if (Stats)
		{
			Stats->AddSuitPower(-Applied * SuitPowerPerWeldPoint);
		}
		Client_WeldFeedback(0, IConstructible::Execute_GetConstructionProgressAt(Target, WorldPoint));
	}
	else
	{
		// Zero progress with budget available: complete target, or the next
		// material unit is missing from the player's inventory.
		Client_WeldFeedback(PhaseNow == EConstructionPhase::Complete ? 3 : 2, 0.f);
	}
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
