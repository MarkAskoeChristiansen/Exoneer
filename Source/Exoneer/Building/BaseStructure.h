// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ExoneerTypes.h"
#include "BaseStructure.generated.h"

class ABasePiece;
class UPieceDefinitionDataAsset;
class UPowerNetworkComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPieceListChanged, ABasePiece*, Piece);

/**
 * One base: the registry and socket graph of its ABasePiece actors, the
 * structural support solver, and the power network host.
 *
 * A structure is born when a groundable piece is placed on terrain. Placing a
 * piece into a socket of an existing piece joins that piece's structure.
 * SERVER owns the socket graph and the solver; clients mirror socket
 * occupancy from each piece's replicated ParentPiece/ParentSocket for
 * previews, and the server remains the placement authority.
 */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API ABaseStructure : public AActor
{
	GENERATED_BODY()

public:
	ABaseStructure();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Structure")
	TObjectPtr<UPowerNetworkComponent> PowerNetwork;

	UPROPERTY(ReplicatedUsing = OnRep_Pieces, VisibleAnywhere, BlueprintReadOnly, Category = "Structure")
	TArray<TObjectPtr<ABasePiece>> Pieces;

	UPROPERTY(BlueprintAssignable) FOnPieceListChanged OnPieceAdded;
	UPROPERTY(BlueprintAssignable) FOnPieceListChanged OnPieceRemoved;

	// --- SERVER placement API (called from the build tool's Server RPCs) ---

	/** Validate a socket placement without committing it. */
	UFUNCTION(BlueprintCallable, Category = "Structure")
	bool CanPlacePiece(UPieceDefinitionDataAsset* Def, ABasePiece* Parent, FName Socket, EBuildPlacementError& OutError) const;

	/** Spawn a GHOST piece snapped into Parent's socket. Null on failure. */
	UFUNCTION(BlueprintCallable, Category = "Structure")
	ABasePiece* PlacePieceGhost(UPieceDefinitionDataAsset* Def, ABasePiece* Parent, FName Socket);

	/**
	 * Spawn a GHOST groundable piece at a world transform, creating the
	 * structure that owns it. STATIC: spawns/uses the right ABaseStructure.
	 */
	static ABasePiece* PlaceGroundedGhost(UWorld* World, UPieceDefinitionDataAsset* Def, const FTransform& Transform, EBuildPlacementError& OutError);

	/** SERVER. Detach bookkeeping when a piece dies or is deconstructed. */
	void NotifyPieceRemoved(ABasePiece* Piece);

	/** SERVER. Piece finished construction: recompute support, register power. */
	void NotifyPieceCompleted(ABasePiece* Piece);

	/** SERVER. Recompute SupportValue for every piece; collapse the unsupported. */
	UFUNCTION(BlueprintCallable, Category = "Structure")
	void RecomputeSupport();

	/** Is Parent's socket already filled? (Surface sockets never fill.) */
	UFUNCTION(BlueprintPure, Category = "Structure")
	bool IsSocketOccupied(const ABasePiece* Parent, FName Socket) const;

	UFUNCTION(BlueprintPure, Category = "Structure")
	int32 GetPieceCount() const { return Pieces.Num(); }

	/** Multicast FX hook fired once per collapse batch. */
	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_OnPiecesCollapsed(const TArray<FVector>& Locations);

	/** BP hook on each machine for collapse FX (dust, debris, audio). */
	UFUNCTION(BlueprintImplementableEvent, Category = "Structure")
	void OnPiecesCollapsedBP(const TArray<FVector>& Locations);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	/** SERVER. Occupied one-shot sockets: key = (parent piece, socket name). */
	TMap<TPair<TWeakObjectPtr<const ABasePiece>, FName>, TWeakObjectPtr<ABasePiece>> OccupiedSockets;

	/** SERVER. Register a freshly spawned piece into registry + socket graph. */
	void RegisterPiece(ABasePiece* Piece, ABasePiece* Parent, FName Socket);

	/**
	 * SERVER. If NewPiece's sockets coincide (within 2 cm) with sockets of
	 * pieces belonging to OTHER structures, merge those structures into this
	 * one (larger absorbs smaller). Returns the surviving structure.
	 */
	ABaseStructure* TryMergeAt(ABasePiece* NewPiece);

	/** Client diff base for OnRep_Pieces, so add/remove events fire off-server too. */
	TArray<TWeakObjectPtr<ABasePiece>> ClientKnownPieces;

	UFUNCTION() void OnRep_Pieces();

	/** SERVER. Adjacent (socket-linked) pieces of P, both directions. */
	void GetLinkedPieces(const ABasePiece* P, TArray<ABasePiece*>& OutLinked) const;

	/** SERVER. Merge Other's pieces/sockets/power into this structure. */
	void AbsorbStructure(ABaseStructure* Other);

	/** SERVER. Overlap test at the prospective piece transform. */
	bool WouldOverlap(UPieceDefinitionDataAsset* Def, const FTransform& Where, const ABasePiece* Ignore = nullptr) const;
};
