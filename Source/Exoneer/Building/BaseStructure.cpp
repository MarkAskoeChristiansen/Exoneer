// Copyright Exoneer contributors.
#include "Building/BaseStructure.h"
#include "Building/BasePiece.h"
#include "Exoneer.h"
#include "Components/ConstructionComponent.h"
#include "Components/PowerComponent.h"
#include "Components/PowerNetworkComponent.h"
#include "Data/PieceDefinitionDataAsset.h"
#include "Machines/MachinePiece.h"
#include "Components/SceneComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

namespace
{
	/** Delay between losing support and the batched destruction, for FX timing. */
	constexpr float CollapseDelaySeconds = 0.5f;

	/** Actor tag marking pieces already scheduled for collapse (no double-batching). */
	const FName NAME_ExoneerCollapsing(TEXT("Exoneer.Collapsing"));

	const FPieceSocketDef* FindSocketDef(const UPieceDefinitionDataAsset* Def, FName Socket)
	{
		if (Def)
		{
			for (const FPieceSocketDef& S : Def->Sockets)
			{
				if (S.SocketName == Socket)
				{
					return &S;
				}
			}
		}
		return nullptr;
	}
}

ABaseStructure::ABaseStructure()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;

	// The structure itself is invisible bookkeeping; a plain scene root gives
	// grounded placements a stable world anchor.
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));

	PowerNetwork = CreateDefaultSubobject<UPowerNetworkComponent>(TEXT("PowerNetwork"));
}

void ABaseStructure::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ABaseStructure, Pieces);
}

// --- Placement ----------------------------------------------------------------

bool ABaseStructure::CanPlacePiece(UPieceDefinitionDataAsset* Def, ABasePiece* Parent, FName Socket, EBuildPlacementError& OutError) const
{
	OutError = EBuildPlacementError::None;

	if (!Def || !Def->MountTag.IsValid())
	{
		OutError = EBuildPlacementError::InvalidDefinition;
		return false;
	}
	if (!IsValid(Parent) || Parent->OwningStructure != this)
	{
		OutError = EBuildPlacementError::NoTarget;
		return false;
	}

	const FPieceSocketDef* SocketDef = FindSocketDef(Parent->Def, Socket);
	if (!SocketDef)
	{
		OutError = EBuildPlacementError::NoTarget;
		return false;
	}
	if (!SocketDef->AcceptedMounts.HasTag(Def->MountTag))
	{
		OutError = EBuildPlacementError::IncompatibleMount;
		return false;
	}
	if (IsSocketOccupied(Parent, Socket))
	{
		OutError = EBuildPlacementError::SocketOccupied;
		return false;
	}

	const FTransform Where = Parent->GetSocketWorldTransform(Socket);
	if (WouldOverlap(Def, Where))
	{
		OutError = EBuildPlacementError::BlockedByCollision;
		return false;
	}

	// Support: the new piece hangs off the parent's chain. Groundable pieces
	// skip the precheck (they may snap on and later count as grounded or draw
	// from neighbors); everything else must end with support left over.
	// Ghost parents hold SupportValue 0, so welding the parent comes first.
	// The formula MUST mirror the solver in RecomputeSupport exactly - a
	// neighbor passes on at most min(its value, its own budget) - our cost -
	// or an approved piece collapses the moment it is welded to Complete.
	if (!Def->bGroundable)
	{
		const int32 ParentBudget = Parent->Def ? Parent->Def->SupportBudget : 0;
		const int32 Prospective = FMath::Min(Parent->SupportValue, ParentBudget) - Def->SupportCost;
		if (Prospective <= 0)
		{
			OutError = EBuildPlacementError::NoSupport;
			return false;
		}
	}
	return true;
}

ABasePiece* ABaseStructure::PlacePieceGhost(UPieceDefinitionDataAsset* Def, ABasePiece* Parent, FName Socket)
{
	if (!HasAuthority())
	{
		return nullptr;
	}

	EBuildPlacementError Error = EBuildPlacementError::None;
	if (!CanPlacePiece(Def, Parent, Socket, Error))
	{
		UE_LOG(LogExoneer, Verbose, TEXT("PlacePieceGhost rejected (%d) for %s"), static_cast<int32>(Error), *GetNameSafe(Def));
		return nullptr;
	}

	const FTransform Where = Parent->GetSocketWorldTransform(Socket);
	UClass* PieceClass = Def->PieceClass ? static_cast<UClass*>(Def->PieceClass) : ABasePiece::StaticClass();

	// Deferred spawn so InitializeGhost runs before BeginPlay and before the
	// first replication update (see the note on ABasePiece::InitializeGhost).
	ABasePiece* Piece = GetWorld()->SpawnActorDeferred<ABasePiece>(PieceClass, Where, this, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Piece)
	{
		return nullptr;
	}
	Piece->InitializeGhost(this, Def, Parent, Socket);
	Piece->FinishSpawning(Where);

	RegisterPiece(Piece, Parent, Socket);
	TryMergeAt(Piece);   // The piece may bridge two structures built toward each other.
	return Piece;
}

ABasePiece* ABaseStructure::PlaceGroundedGhost(UWorld* World, UPieceDefinitionDataAsset* Def, const FTransform& Transform, EBuildPlacementError& OutError)
{
	OutError = EBuildPlacementError::None;

	if (!World || World->GetNetMode() == NM_Client)
	{
		OutError = EBuildPlacementError::Unknown;
		return nullptr;
	}
	if (!Def || !Def->bGroundable)
	{
		OutError = EBuildPlacementError::InvalidDefinition;
		return nullptr;
	}

	// The caller (build tool server RPC) already fitted the transform to the
	// terrain (surface snap, slope limit); this only guards world overlap and
	// founds the structure that will own the piece.
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ABaseStructure* Structure = World->SpawnActor<ABaseStructure>(ABaseStructure::StaticClass(), Transform, Params);
	if (!Structure)
	{
		OutError = EBuildPlacementError::Unknown;
		return nullptr;
	}

	if (Structure->WouldOverlap(Def, Transform))
	{
		Structure->Destroy();
		OutError = EBuildPlacementError::BlockedByCollision;
		return nullptr;
	}

	UClass* PieceClass = Def->PieceClass ? static_cast<UClass*>(Def->PieceClass) : ABasePiece::StaticClass();
	ABasePiece* Piece = World->SpawnActorDeferred<ABasePiece>(PieceClass, Transform, Structure, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Piece)
	{
		Structure->Destroy();
		OutError = EBuildPlacementError::Unknown;
		return nullptr;
	}
	Piece->InitializeGhost(Structure, Def, nullptr, NAME_None);
	Piece->FinishSpawning(Transform);

	Structure->RegisterPiece(Piece, nullptr, NAME_None);
	Structure->TryMergeAt(Piece);   // Grounded next to an existing base: same structure.
	return Piece;
}

void ABaseStructure::RegisterPiece(ABasePiece* Piece, ABasePiece* Parent, FName Socket)
{
	if (!Piece)
	{
		return;
	}

	Pieces.Add(Piece);

	// Cross-structure parents cannot happen through CanPlacePiece; merging
	// goes through AbsorbStructure explicitly, never silent re-registration.
	if (Parent && !Socket.IsNone() && Parent->OwningStructure == this)
	{
		const FPieceSocketDef* SocketDef = FindSocketDef(Parent->Def, Socket);
		if (SocketDef && !SocketDef->bSurfaceSocket)
		{
			OccupiedSockets.Add(TPair<TWeakObjectPtr<const ABasePiece>, FName>(Parent, Socket), Piece);
		}
	}

	OnPieceAdded.Broadcast(Piece);
}

bool ABaseStructure::IsSocketOccupied(const ABasePiece* Parent, FName Socket) const
{
	// Surface sockets (floors hosting deployables) accept any number of
	// pieces and never fill.
	const FPieceSocketDef* SocketDef = Parent ? FindSocketDef(Parent->Def, Socket) : nullptr;
	if (!SocketDef || SocketDef->bSurfaceSocket)
	{
		return false;
	}

	const TWeakObjectPtr<ABasePiece>* Occupant = OccupiedSockets.Find(TPair<TWeakObjectPtr<const ABasePiece>, FName>(Parent, Socket));
	return Occupant && Occupant->IsValid();
}

bool ABaseStructure::WouldOverlap(UPieceDefinitionDataAsset* Def, const FTransform& Where, const ABasePiece* Ignore) const
{
	UWorld* World = GetWorld();
	if (!Def || !World)
	{
		return false;
	}

	// Box test from the authored mesh bounds, shrunk slightly so pieces that
	// merely touch at socket seams do not reject each other.
	UStaticMesh* MeshAsset = Def->Mesh.LoadSynchronous();
	if (!MeshAsset)
	{
		return false;
	}
	const FBox Bounds = MeshAsset->GetBoundingBox();
	const FVector Extent = Bounds.GetExtent() * Where.GetScale3D() * 0.9f;
	// Pieces render bottom-aligned (origin = mount point), so the overlap box
	// must test the shifted volume, not the raw centered mesh bounds.
	const FVector AlignedCenter = Bounds.GetCenter() - FVector(0.f, 0.f, Bounds.Min.Z);
	const FVector Center = Where.TransformPosition(AlignedCenter);

	FCollisionQueryParams Params(FName(TEXT("ExoneerPieceOverlap")), /*bTraceComplex*/ false);
	if (Ignore)
	{
		Params.AddIgnoredActor(Ignore);
	}

	TArray<FOverlapResult> Overlaps;
	World->OverlapMultiByChannel(Overlaps, Center, Where.GetRotation(), ECC_WorldStatic, FCollisionShape::MakeBox(Extent), Params);

	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* Other = Overlap.GetActor();
		if (!Other || Other == Ignore)
		{
			continue;
		}
		// Ghost pieces DO block placement: two coincident ghosts would both
		// weld into the same space and end Complete inside each other. The 10%
		// bounds shrink above already keeps legitimate socket-seam neighbors
		// from rejecting each other.
		return true;
	}
	return false;
}

// --- Lifecycle bookkeeping ------------------------------------------------------

void ABaseStructure::NotifyPieceCompleted(ABasePiece* Piece)
{
	if (!HasAuthority() || !Piece)
	{
		return;
	}

	// A completed machine joins the structure's power network (the solver's
	// discipline pass would also catch it; this keeps the path explicit).
	if (AMachinePiece* Machine = Cast<AMachinePiece>(Piece))
	{
		if (PowerNetwork && Machine->Power)
		{
			PowerNetwork->Register(Machine->Power);
		}
	}

	RecomputeSupport();
}

void ABaseStructure::NotifyPieceRemoved(ABasePiece* Piece)
{
	if (!HasAuthority() || !Piece)
	{
		return;
	}
	if (Pieces.Remove(Piece) == 0)
	{
		return; // Not ours, or already detached.
	}

	if (AMachinePiece* Machine = Cast<AMachinePiece>(Piece))
	{
		if (PowerNetwork && Machine->Power)
		{
			PowerNetwork->Unregister(Machine->Power);
		}
	}

	// Drop socket bookkeeping where the piece is the parent or the occupant.
	for (auto It = OccupiedSockets.CreateIterator(); It; ++It)
	{
		if (It->Key.Key == Piece || It->Value == Piece)
		{
			It.RemoveCurrent();
		}
	}

	OnPieceRemoved.Broadcast(Piece);
	RecomputeSupport();

	// A structure with no pieces left is dead bookkeeping: an invisible,
	// replicated actor with a ticking power network. Deferred destroy - the
	// collapse lambda may still be on the stack and about to multicast.
	if (Pieces.Num() == 0)
	{
		SetLifeSpan(0.2f);
	}
}

void ABaseStructure::GetLinkedPieces(const ABasePiece* P, TArray<ABasePiece*>& OutLinked) const
{
	OutLinked.Reset();
	if (!P)
	{
		return;
	}

	// Upstream: the piece this one snapped into.
	if (IsValid(P->ParentPiece))
	{
		OutLinked.Add(P->ParentPiece);
	}

	// Downstream: every piece parented to P. Scanning ParentPiece instead of
	// OccupiedSockets also covers surface-socket children (deployed machines),
	// which never create occupancy entries but still need support.
	for (const TObjectPtr<ABasePiece>& Other : Pieces)
	{
		if (IsValid(Other) && Other != P && Other->ParentPiece == P)
		{
			OutLinked.Add(Other);
		}
	}
}

void ABaseStructure::AbsorbStructure(ABaseStructure* Other)
{
	if (!HasAuthority() || !Other || Other == this)
	{
		return;
	}

	TArray<ABasePiece*> Moved;
	Moved.Reserve(Other->Pieces.Num());
	for (const TObjectPtr<ABasePiece>& P : Other->Pieces)
	{
		if (IsValid(P))
		{
			Moved.Add(P);
		}
	}
	Other->Pieces.Reset();

	for (ABasePiece* P : Moved)
	{
		P->OwningStructure = this;
		Pieces.Add(P);

		// Completed machines switch power networks with their piece.
		if (AMachinePiece* Machine = Cast<AMachinePiece>(P))
		{
			if (Machine->Power)
			{
				if (Other->PowerNetwork)
				{
					Other->PowerNetwork->Unregister(Machine->Power);
				}
				if (PowerNetwork && Machine->Construction && Machine->Construction->IsComplete())
				{
					PowerNetwork->Register(Machine->Power);
				}
			}
		}
		OnPieceAdded.Broadcast(P);
	}

	OccupiedSockets.Append(Other->OccupiedSockets);
	Other->OccupiedSockets.Empty();

	Other->Destroy();
	RecomputeSupport();
}

// --- Support solver -------------------------------------------------------------

void ABaseStructure::RecomputeSupport()
{
	if (!HasAuthority())
	{
		return;
	}

	// Drop dead references before solving.
	Pieces.RemoveAll([](const TObjectPtr<ABasePiece>& P) { return !IsValid(P); });

	// Power-registration discipline: only COMPLETE machines feed the network.
	// Register/Unregister are idempotent, so enforcing the invariant on every
	// solve also handles deconstruction demoting a completed machine.
	if (PowerNetwork)
	{
		for (const TObjectPtr<ABasePiece>& P : Pieces)
		{
			if (AMachinePiece* Machine = Cast<AMachinePiece>(P.Get()))
			{
				if (Machine->Power)
				{
					if (Machine->Construction && Machine->Construction->IsComplete())
					{
						PowerNetwork->Register(Machine->Power);
					}
					else
					{
						PowerNetwork->Unregister(Machine->Power);
					}
				}
			}
		}
	}

	// Reset. Ghosts stay at 0 by design: they take no support, give none, and
	// never collapse.
	for (const TObjectPtr<ABasePiece>& P : Pieces)
	{
		P->SupportValue = 0;
	}

	// Seeds: grounded, completed pieces radiate their own budget.
	TArray<ABasePiece*> Queue;
	Queue.Reserve(Pieces.Num());
	for (const TObjectPtr<ABasePiece>& P : Pieces)
	{
		if (P->Def && P->Def->bGroundable && !P->ParentPiece && P->Construction && P->Construction->IsComplete())
		{
			P->SupportValue = P->Def->SupportBudget;
			Queue.Add(P);
		}
	}

	// Relaxation BFS over socket links. A neighbor passes on at most its own
	// budget minus the receiving piece's cost; values only rise, so each piece
	// re-enters the queue a bounded number of times. Full per-structure passes
	// are fine at prototype scale (< 1000 pieces).
	TArray<ABasePiece*> Linked;
	for (int32 Head = 0; Head < Queue.Num(); ++Head)
	{
		ABasePiece* Node = Queue[Head];
		if (!IsValid(Node) || !Node->Def)
		{
			continue;
		}
		GetLinkedPieces(Node, Linked);
		for (ABasePiece* P : Linked)
		{
			if (!IsValid(P) || !P->Def || !P->Construction)
			{
				continue;
			}
			if (P->Construction->GetPhase() == EConstructionPhase::Ghost)
			{
				continue;
			}
			const int32 Candidate = FMath::Min(Node->SupportValue, Node->Def->SupportBudget) - P->Def->SupportCost;
			if (Candidate > P->SupportValue)
			{
				P->SupportValue = Candidate;
				Queue.Add(P);
			}
		}
	}

	// Collapse batch: unsupported pieces of ANY built phase, plus orphans.
	// Ghosts persist while their parent lives (they are planning markers), but
	// a non-groundable piece whose parent actor is gone must fall regardless
	// of phase, or it floats in midair forever. The actor tag prevents
	// re-batching a piece already waiting on its timer.
	TArray<TWeakObjectPtr<ABasePiece>> Batch;
	for (const TObjectPtr<ABasePiece>& P : Pieces)
	{
		if (!P->Def || !P->Construction)
		{
			continue;
		}
		const bool bOrphaned = !P->Def->bGroundable && !IsValid(P->ParentPiece);
		if (P->Construction->GetPhase() == EConstructionPhase::Ghost && !bOrphaned)
		{
			continue;
		}
		if (P->Def->bGroundable && !P->ParentPiece)
		{
			continue;
		}
		if ((!bOrphaned && P->SupportValue > 0) || P->Tags.Contains(NAME_ExoneerCollapsing))
		{
			continue;
		}
		P->Tags.Add(NAME_ExoneerCollapsing);
		Batch.Add(P.Get());
	}

	if (Batch.Num() > 0)
	{
		// Short delay so the multicast FX and the disappearance read as one
		// event. Cascades run wave by wave: destroying this batch recomputes
		// support, which may schedule the next batch.
		FTimerHandle Handle;
		GetWorldTimerManager().SetTimer(Handle, FTimerDelegate::CreateWeakLambda(this, [this, Batch]()
		{
			TArray<FVector> Locations;
			Locations.Reserve(Batch.Num());
			for (const TWeakObjectPtr<ABasePiece>& Weak : Batch)
			{
				if (ABasePiece* P = Weak.Get())
				{
					Locations.Add(P->GetActorLocation());
					NotifyPieceRemoved(P);
					P->Destroy();
				}
			}
			if (Locations.Num() > 0)
			{
				Multicast_OnPiecesCollapsed(Locations);
			}
		}), CollapseDelaySeconds, false);
	}
}

void ABaseStructure::Multicast_OnPiecesCollapsed_Implementation(const TArray<FVector>& Locations)
{
	OnPiecesCollapsedBP(Locations);
}

void ABaseStructure::OnRep_Pieces()
{
	// Diff against the last known set so client HUD/BP hears the same
	// add/remove events the listen host gets from RegisterPiece/NotifyPieceRemoved.
	for (const TObjectPtr<ABasePiece>& P : Pieces)
	{
		if (IsValid(P) && !ClientKnownPieces.Contains(P))
		{
			OnPieceAdded.Broadcast(P);
		}
	}
	for (const TWeakObjectPtr<ABasePiece>& Known : ClientKnownPieces)
	{
		if (Known.IsValid() && !Pieces.Contains(Known.Get()))
		{
			OnPieceRemoved.Broadcast(Known.Get());
		}
	}
	ClientKnownPieces.Reset(Pieces.Num());
	for (const TObjectPtr<ABasePiece>& P : Pieces)
	{
		if (IsValid(P))
		{
			ClientKnownPieces.Add(P);
		}
	}
}

ABaseStructure* ABaseStructure::TryMergeAt(ABasePiece* NewPiece)
{
	if (!HasAuthority() || !IsValid(NewPiece) || !NewPiece->Def)
	{
		return this;
	}

	// Collect the new piece's socket anchor points in world space.
	TArray<FVector> MySocketPoints;
	MySocketPoints.Reserve(NewPiece->Def->Sockets.Num() + 1);
	MySocketPoints.Add(NewPiece->GetActorLocation());
	for (const FPieceSocketDef& SocketDef : NewPiece->Def->Sockets)
	{
		MySocketPoints.Add(NewPiece->GetSocketWorldTransform(SocketDef.SocketName).GetLocation());
	}

	// Any OTHER structure exposing a socket that lands on one of ours (or on
	// our origin) within 2 cm is the same physical base - merge. Larger
	// structure absorbs smaller so the fewest pieces move.
	constexpr float MergeToleranceSq = 2.f * 2.f;
	ABaseStructure* Survivor = this;
	bool bMergedAny = true;
	while (bMergedAny)
	{
		bMergedAny = false;
		for (TActorIterator<ABaseStructure> It(GetWorld()); It; ++It)
		{
			ABaseStructure* Other = *It;
			if (Other == Survivor || !IsValid(Other))
			{
				continue;
			}
			bool bTouches = false;
			for (const TObjectPtr<ABasePiece>& OtherPiece : Other->Pieces)
			{
				if (!IsValid(OtherPiece) || !OtherPiece->Def)
				{
					continue;
				}
				for (const FPieceSocketDef& SocketDef : OtherPiece->Def->Sockets)
				{
					const FVector OtherPoint = OtherPiece->GetSocketWorldTransform(SocketDef.SocketName).GetLocation();
					for (const FVector& MyPoint : MySocketPoints)
					{
						if (FVector::DistSquared(OtherPoint, MyPoint) <= MergeToleranceSq)
						{
							bTouches = true;
							break;
						}
					}
					if (bTouches) break;
				}
				if (bTouches) break;
			}
			if (!bTouches)
			{
				continue;
			}

			if (Other->GetPieceCount() >= Survivor->GetPieceCount())
			{
				Other->AbsorbStructure(Survivor);
				Survivor = Other;
			}
			else
			{
				Survivor->AbsorbStructure(Other);
			}
			bMergedAny = true;
			break; // The iterator is invalidated by the destroy; rescan.
		}
	}
	return Survivor;
}
