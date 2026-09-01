// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SaveGameSubsystem.generated.h"

class UExoneerSaveGame;
class UConstructionComponent;
class UPieceDefinitionDataAsset;
class UVehicleBlockDefinitionDataAsset;
class ABasePiece;
class AVehicleConstruct;
struct FSavedBasePiece;
struct FSavedStructure;
struct FSavedVehicle;
struct FSavedVehicleBlock;

/**
 * High-level save/load entry point. SERVER-ONLY: the server owns all world
 * state, and loading recreates it server-side so replication rebuilds the
 * clients (spec section 12). Calls on a client log a warning and fail.
 *
 * Save walks the world (player, ABaseStructure pieces, AVehicleConstruct
 * block records, environment manager) into an UExoneerSaveGame. Load clears
 * the built world, resolves definitions through the AssetManager primary
 * asset ids ("Piece:<PieceId>" / "VehicleBlock:<BlockId>"), and respawns
 * everything through the normal ghost placement APIs before restoring
 * phase/progress/health per piece and block.
 */
UCLASS(BlueprintType)
class EXONEER_API USaveGameSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "Save") bool SaveToSlot(const FString& SlotName);
	UFUNCTION(BlueprintCallable, Category = "Save") bool LoadFromSlot(const FString& SlotName);
	UFUNCTION(BlueprintCallable, Category = "Save") bool HasSave(const FString& SlotName) const;

protected:
	/** True when this world may mutate save state (listen host / standalone). */
	bool EnsureServer(const TCHAR* Operation) const;

	UExoneerSaveGame* GatherWorldState() const;
	void ApplyWorldState(UExoneerSaveGame* Save);

	// --- Load internals (SERVER) ---

	/** Destroy every existing piece, structure, and vehicle construct. */
	void ClearBuiltWorld(UWorld* World) const;

	void ApplyStructures(UWorld* World, const UExoneerSaveGame* Save) const;
	void ApplyVehicles(UWorld* World, const UExoneerSaveGame* Save) const;

	/** Resolve a definition by primary asset id, sync-loading when needed. */
	UPieceDefinitionDataAsset* ResolvePieceDef(FName PieceId) const;
	UVehicleBlockDefinitionDataAsset* ResolveVehicleBlockDef(FName BlockId) const;

	/**
	 * Spec 12 re-link rule: among the already respawned pieces, find the
	 * nearest free socket accepting Def's mount whose world location lies
	 * within 1 cm of the saved piece transform.
	 */
	static bool FindReLinkSocket(const TArray<ABasePiece*>& Spawned, const UPieceDefinitionDataAsset* Def,
		const FTransform& SavedTransform, ABasePiece*& OutParent, FName& OutSocket);

	/** Restore phase/progress/health (+ machine inventory/energy) on a piece. */
	static void RestorePieceState(ABasePiece* Piece, const FSavedBasePiece& Saved);

	/** Write construction phase/stage/progress back onto a fresh ghost. */
	static void RestoreConstructionState(UConstructionComponent* Construction, uint8 Phase, int32 StageIndex, float StageProgress01);

	/** Restore one vehicle block record's saved fields by BlockInstanceId. */
	static void RestoreVehicleBlockRecord(AVehicleConstruct* Construct, int32 BlockInstanceId, const FSavedVehicleBlock& Saved, bool bRestoreOrientation);
};
