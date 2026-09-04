// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "ExoneerTypes.h"
#include "PieceDefinitionDataAsset.generated.h"

class ABasePiece;
class UStaticMesh;
class UTexture2D;

/**
 * One socket a placed piece exposes. A held piece may snap here when the
 * socket's AcceptedMounts contains the held piece's MountTag.
 */
USTRUCT(BlueprintType)
struct FPieceSocketDef
{
	GENERATED_BODY()

	/** Unique within the piece ("Edge_N", "Top_0", "Surface_Center", ...). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Socket")
	FName SocketName;

	/** Where a mounted piece's origin lands, relative to the piece origin. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Socket")
	FTransform LocalTransform;

	/** Which mount tags (Exoneer.Mount.*) may snap into this socket. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Socket", meta = (Categories = "Exoneer.Mount"))
	FGameplayTagContainer AcceptedMounts;

	/**
	 * Surface sockets (floors hosting deployables) accept many pieces and are
	 * never marked occupied; edge sockets accept exactly one piece.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Socket")
	bool bSurfaceSocket = false;
};

/**
 * Data-driven description of one base building piece (foundation, wall, floor,
 * ramp, roof, beam, door frame, or a deployable machine). Author instances
 * under /Content/Exoneer/Data/Pieces/.
 */
UCLASS(BlueprintType)
class EXONEER_API UPieceDefinitionDataAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Stable identifier ("foundation_salvage", "wall_alloy", "refinery", ...). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AssetRegistrySearchable, Category = "Piece")
	FName PieceId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Piece")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Piece")
	EStructureTier Tier = EStructureTier::Salvage;

	/** What this piece is, for socket matching (exactly one Exoneer.Mount.* tag). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Piece", meta = (Categories = "Exoneer.Mount"))
	FGameplayTag MountTag;

	/** Sockets this piece exposes once placed. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Piece")
	TArray<FPieceSocketDef> Sockets;

	/** Ghost -> complete investment stages (materials + weld work). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Construction")
	TArray<FConstructionCost> Stages;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Stats")
	float MaxHealth = 200.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Stats")
	float Mass = 100.f;

	/** Support units this piece passes downstream when grounded or supported. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Support")
	int32 SupportBudget = 6;

	/** Support units this piece consumes from its parent chain. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Support")
	int32 SupportCost = 1;

	/** May snap directly to terrain and counts as grounded (foundations, beams). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Support")
	bool bGroundable = false;

	/**
	 * Live wheel load this piece carries, as a MASS (kg): the weight limit is
	 * LoadCapacityKg * g on this planet. 0 means UNRATED - the piece is not a
	 * deck and collapses under any wheel at all (V-SPAN, GAME-SCOPE 10).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Support")
	float LoadCapacityKg = 0.f;

	/**
	 * Permanent set (mm) at which the deck is condemned: it stays standing and
	 * keeps passing support, but carries nothing and the legal verb is rebuild.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Support")
	float TerminalDeflectionMm = 60.f;

	/** 0..1 storm damage mitigation. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Stats", meta = (ClampMin = "0", ClampMax = "1"))
	float StormResistance = 0.f;

	// --- Machine stats (used when PieceClass is an AMachinePiece subclass) ---

	/** Watts. Positive produces, negative consumes. 0 for pure architecture. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Machine")
	float PowerDelta = 0.f;

	/** Watt-seconds of storage (battery machines). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Machine")
	float EnergyStorage = 0.f;

	/** Internal buffer capacity in volume units (0 = no inventory). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Machine")
	float InventoryCapacity = 0.f;

	/** Oxygen units per second produced while powered (oxygen generators). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Machine")
	float OxygenProductionPerSec = 0.f;

	/**
	 * Primary asset name of the fabricated spare the Replace verb consumes
	 * (battery bank: "battery_cell"). None means this piece has no spare and
	 * can never be replaced - the verb for it is rebuild. Mirrors
	 * UVehicleBlockDefinitionDataAsset::SpareItemId.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Machine")
	FName SpareItemId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Visual")
	TSoftObjectPtr<UStaticMesh> Mesh;

	/** Optional dedicated ghost mesh; falls back to Mesh with ghost material. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Visual")
	TSoftObjectPtr<UStaticMesh> GhostMesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Visual")
	TSoftObjectPtr<UTexture2D> Icon;

	/** ABasePiece for architecture; AMachinePiece subclasses for machines. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Piece")
	TSubclassOf<ABasePiece> PieceClass;

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId(TEXT("Piece"), PieceId);
	}
};
