// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/NetSerialization.h"
#include "ExoneerTypes.h"
#include "Interfaces/Constructible.h"   // ExoneerConstruction::NoTargetId, read by the HUD
#include "BuildToolComponent.generated.h"

class UPieceDefinitionDataAsset;
class UVehicleBlockDefinitionDataAsset;
class ABasePiece;
class ABaseStructure;
class AVehicleConstruct;
class UStaticMeshComponent;
class UMaterialInterface;
class UPrimaryDataAsset;

UENUM(BlueprintType)
enum class EBuildToolMode : uint8
{
	None,
	BasePlacement,     // Socket-snapped architectural pieces
	VehiclePlacement   // Grid-snapped vehicle blocks
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnBuildPreviewChanged, bool, bValid, EBuildPlacementError, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnSelectedBuildableChanged, UPrimaryDataAsset*, NewSelection);

/**
 * Player build/weld tool.
 *
 * Placement is ghost-first: confirming spawns a GHOST (free), which is then
 * finished by welding (hold primary action in Weld tool mode) that consumes
 * materials from the player inventory. Secondary action deconstructs/refunds.
 *
 * The ghost PREVIEW is purely client-side; every commit goes through a Server
 * RPC on this (connection-owned) component and is fully revalidated by
 * ABaseStructure / AVehicleConstruct on the server.
 */
UCLASS(ClassGroup = (Exoneer), meta = (BlueprintSpawnableComponent))
class EXONEER_API UBuildToolComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UBuildToolComponent();

	// --- Tunables ---
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Build") float PlacementRange = 800.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Build") float TerrainSlopeLimitDeg = 25.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weld") float WeldPointsPerSec = 10.f;
	/** Suit power per weld-second (kJ). 1.8 kJ keeps weld cost identical to the old 0.1/100 units. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weld") float SuitPowerPerWeldPoint = 1.8f;

	/** Weld aim sweep radius (cm); generous because blocks can be 25 cm. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weld") float WeldAimRadius = 14.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Build") TSoftObjectPtr<UMaterialInterface> ValidPreviewMaterial;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Build") TSoftObjectPtr<UMaterialInterface> InvalidPreviewMaterial;

	// --- Events (HUD) ---
	UPROPERTY(BlueprintAssignable) FOnBuildPreviewChanged OnBuildPreviewChanged;
	UPROPERTY(BlueprintAssignable) FOnSelectedBuildableChanged OnSelectedBuildableChanged;

	// --- Mode & selection ---
	UFUNCTION(BlueprintCallable, Category = "Build") void SetBuildModeEnabled(bool bEnabled);
	UFUNCTION(BlueprintPure, Category = "Build") bool IsBuildModeEnabled() const { return Mode != EBuildToolMode::None; }
	UFUNCTION(BlueprintPure, Category = "Build") EBuildToolMode GetMode() const { return Mode; }

	UFUNCTION(BlueprintCallable, Category = "Build") void SetSelectedPiece(UPieceDefinitionDataAsset* Piece);
	UFUNCTION(BlueprintCallable, Category = "Build") void SetSelectedVehicleBlock(UVehicleBlockDefinitionDataAsset* Block);
	UFUNCTION(BlueprintPure, Category = "Build") UPrimaryDataAsset* GetSelected() const;
	UFUNCTION(BlueprintPure, Category = "Build") EBuildPlacementError GetLastPreviewError() const { return LastError; }

	// --- Last weld feedback, for the visor HUD (written by Client_WeldFeedback). ---
	/** 0 progressed, 1 no suit power, 2 missing materials, 3 complete, 4 no target, 255 none. */
	UPROPERTY(BlueprintReadOnly, Category = "Weld") uint8 LastWeldResult = 255;
	UPROPERTY(BlueprintReadOnly, Category = "Weld") float LastWeldProgress01 = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Weld") float LastWeldFeedbackSeconds = -1000.f;

	/**
	 * Build progress of the part under the beam, sampled EVERY FRAME on the
	 * owning client straight off the replicated construction state.
	 *
	 * Client_WeldFeedback only arrives once per RPC flush (5 Hz), so a readout
	 * driven from LastWeldProgress01 climbs in five visible jumps a second.
	 * This is the same fraction of the same work, read at frame rate, so the
	 * percentage counts through every value between 0 and 100.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Weld") float LiveWeldProgress01 = 0.f;

	/** True while LiveWeldProgress01 describes a real constructible under the beam. */
	UPROPERTY(BlueprintReadOnly, Category = "Weld") bool bLiveWeldTargetValid = false;

	/**
	 * Identity of the construction target the last weld feedback described -
	 * the block that ACTUALLY took the work, not whatever the aim point
	 * resolved to. ExoneerConstruction::NoTargetId (INDEX_NONE) whenever no
	 * work landed, and then there is no progress number to show at all.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Weld") int32 LastWeldTargetId = INDEX_NONE;

	/** R key: cycle vehicle block orientation / base piece socket alternative. */
	UFUNCTION(BlueprintCallable, Category = "Build") void CycleOrientation(int32 Steps = 1);

	/** Human label for the current aim of the selected vehicle block ("THRUST: UP", "YAW 90"); empty when not applicable. */
	UFUNCTION(BlueprintPure, Category = "Build") FString GetOrientationLabel() const;

	// --- Actions (client entry points) ---
	UFUNCTION(BlueprintCallable, Category = "Build") bool TryConfirmPlacement();

	/** Hold-to-weld / hold-to-deconstruct; driven by primary/secondary action. */
	UFUNCTION(BlueprintCallable, Category = "Weld") void SetWeldActive(bool bActive);
	UFUNCTION(BlueprintCallable, Category = "Weld") void SetDeconstructActive(bool bActive);

	/**
	 * SERVER. The whole weld decision for one batch: invest into an unfinished
	 * ghost, else wipe or replace on a Complete part. It never restores health
	 * or any other reading (GAME-SCOPE §10). Server_Weld forwards straight
	 * here, and the automation suite calls it directly so the no-weld-heal
	 * contract is asserted on the real routing rather than on a stub.
	 */
	/**
	 * THE WELD RULE. While the part under the beam is Complete a held weld
	 * stream does nothing at all: every batch after the first of a press can
	 * only ever INVEST work, so wipe and replace - the verbs that spend a
	 * fabricated spare or clear a surface - are reachable only from the first
	 * batch of a deliberate new press, never from the tail of the stream that
	 * just finished the weld. bFreshPress carries that press boundary; it
	 * defaults to true so a direct call (the automation suite) reads as one
	 * deliberate press.
	 */
	void ServerApplyWeld(AActor* Target, const FVector& WorldPoint, float WeldPoints, bool bFreshPress = true);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn) override;

protected:
	UPROPERTY(VisibleInstanceOnly, Category = "Build") EBuildToolMode Mode = EBuildToolMode::None;
	UPROPERTY() TObjectPtr<UPieceDefinitionDataAsset> SelectedPiece;
	UPROPERTY() TObjectPtr<UVehicleBlockDefinitionDataAsset> SelectedBlock;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> PreviewMesh;

	// Current client-side preview candidate.
	UPROPERTY() TObjectPtr<ABasePiece> CandidateParent;
	FName CandidateSocket;
	FTransform CandidateGroundTransform;
	bool bCandidateGrounded = false;
	UPROPERTY() TObjectPtr<AVehicleConstruct> CandidateConstruct;
	FIntVector CandidateCell = FIntVector::ZeroValue;
	uint8 Orientation = 0;

	bool bWeldActive = false;
	bool bDeconstructActive = false;

	/**
	 * Press bookkeeping for the weld rule above. The client stamps every batch
	 * with the id of the press that produced it; the server treats a batch
	 * whose id it has not seen as the first batch of a new press and every
	 * later batch as the tail of a hold. The old guard keyed on
	 * (target, block id) failed open whenever the sweep point resolved to a
	 * different block between two flushes - which the impact point does on
	 * every aim wobble, and again when a finished block swaps its collision.
	 */
	/**
	 * Client: the actor the last flushed weld batch was sent to. It pairs with
	 * LastWeldTargetId, which names a target INSIDE that actor, so the live
	 * per-frame sample can only be trusted while the beam is still on it.
	 */
	UPROPERTY(Transient) TWeakObjectPtr<AActor> WeldFeedbackTarget;

	uint8 WeldPressId = 0;              // Client: bumped on each fresh press.
	bool bWeldPressBatchPending = false; // Client: this press has not flushed yet.
	uint8 ServerWeldPressId = 0;        // Server: id of the press last seen.
	bool bServerWeldPressSeen = false;  // Server: ServerWeldPressId is meaningful.
	bool bLastPreviewValid = false;
	EBuildPlacementError LastError = EBuildPlacementError::None;
	float WeldRpcAccumulator = 0.f;

	// --- Client preview internals ---
	void EnsurePreviewMesh();
	void UpdatePreview();
	void UpdateBasePreview(const FHitResult& Hit);
	void UpdateVehiclePreview(const FHitResult& Hit);
	void SetPreviewState(bool bValid, EBuildPlacementError Error, const FTransform& Where);
	void ClearPreview();
	bool AimTrace(FHitResult& OutHit) const;

	/** Forgiving weld aim: a sphere sweep, since blocks can be only 25 cm. */
	bool WeldAimSweep(FHitResult& OutHit) const;

	void TickWeldBeam(float DeltaTime);

	// --- Commit RPCs (validated server-side; see ABaseStructure/AVehicleConstruct) ---
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_PlaceBasePiece(UPieceDefinitionDataAsset* Def, ABasePiece* Parent, FName Socket);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_PlaceGroundedPiece(UPieceDefinitionDataAsset* Def, FTransform Transform);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_PlaceVehicleBlock(AVehicleConstruct* Construct, UVehicleBlockDefinitionDataAsset* Def, FIntVector Origin, uint8 InOrientation);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_FoundVehicleConstruct(UVehicleBlockDefinitionDataAsset* Def, FTransform Transform, uint8 InOrientation);

	/** Tells the initiating client why the server refused a placement (HUD feedback). */
	UFUNCTION(Client, Reliable)
	void Client_PlacementRejected(EBuildPlacementError Error);

	/**
	 * Weld tick result for on-screen feedback: 0 progressed, 1 no suit power,
	 * 2 missing materials, 3 already complete.
	 *
	 * TargetId names the construction target the work went into, and is
	 * ExoneerConstruction::NoTargetId for every result except 0 - no work, no
	 * identity, no progress number.
	 */
	UFUNCTION(Client, Unreliable)
	void Client_WeldFeedback(uint8 Result, float Progress01, int32 TargetId);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_Weld(AActor* Target, FVector_NetQuantize WorldPoint, float WeldPoints, uint8 PressId);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_Deconstruct(AActor* Target, FVector_NetQuantize WorldPoint, float WreckPoints);

	/** SERVER helpers shared by the RPC implementations. */
	bool ServerValidateReach(const FVector& Point) const;
	bool ServerValidateGrounded(const FTransform& Transform) const;
	class UInventoryComponent* GetOwnerInventory() const;
};
