// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "ExoneerTypes.h"
#include "Constructible.generated.h"

class UInventoryComponent;

/**
 * Identity of ONE construction target inside a constructible actor.
 *
 * An actor can hold many independent targets (a vehicle construct holds one
 * per block record), so "the target at this world point" is not enough to
 * name the thing a weld actually went into: the weld aim sweep has a 14 cm
 * radius and a block is 25 cm, so the point that resolved the target often
 * lands on the finished NEIGHBOUR of the ghost being welded. Work is
 * therefore reported back by identity, and progress is read back by the same
 * identity, so a readout always follows the thing being welded.
 */
namespace ExoneerConstruction
{
	/** Whole-actor constructibles (ABasePiece): the actor itself is the one target. */
	inline constexpr int32 WholeActorTargetId = 0;

	/** No target: nothing to weld here, and nothing to show a progress number for. */
	inline constexpr int32 NoTargetId = INDEX_NONE;

	/** GetConstructionProgressForTarget answers this when the identity names nothing. */
	inline constexpr float UnknownProgress = -1.f;
}

UINTERFACE(BlueprintType, MinimalAPI)
class UConstructible : public UInterface { GENERATED_BODY() };

/**
 * The ghost-then-invest construction lifecycle, implemented by ABasePiece
 * (whole-actor construction) and AVehicleConstruct (per-block construction,
 * addressed by WorldPoint).
 *
 * All mutating functions are SERVER-ONLY; the build tool routes client intent
 * through its own Server RPCs and then calls these on authority.
 */
class EXONEER_API IConstructible
{
	GENERATED_BODY()

public:
	/** Phase of the construction target nearest WorldPoint. */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Construction")
	EConstructionPhase GetConstructionPhaseAt(const FVector& WorldPoint) const;
	virtual EConstructionPhase GetConstructionPhaseAt_Implementation(const FVector& WorldPoint) const { return EConstructionPhase::Complete; }

	/**
	 * Total 0..1 progress of the construction target nearest WorldPoint.
	 *
	 * NOTE the ambiguity this carries by design: it answers 1.0 both for a
	 * Complete target and for a point that resolves to NO target at all. That
	 * is fine for a coarse "is there anything to do here" question, and it is
	 * exactly why a weld READOUT must not use it - use
	 * GetConstructionProgressForTarget with the identity the weld reported.
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Construction")
	float GetConstructionProgressAt(const FVector& WorldPoint) const;
	virtual float GetConstructionProgressAt_Implementation(const FVector& WorldPoint) const { return 1.f; }

	/**
	 * Total 0..1 progress of the target with this identity, or
	 * ExoneerConstruction::UnknownProgress (negative) when the identity names
	 * nothing here. Unambiguous by construction: a caller that gets a negative
	 * answer shows NO progress rather than inventing 100 percent.
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Construction")
	float GetConstructionProgressForTarget(int32 TargetId) const;
	virtual float GetConstructionProgressForTarget_Implementation(int32 TargetId) const { return ExoneerConstruction::UnknownProgress; }

	/** Identity of the construction target nearest WorldPoint, NoTargetId for none. */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Construction")
	int32 GetConstructionTargetIdAt(const FVector& WorldPoint) const;
	virtual int32 GetConstructionTargetIdAt_Implementation(const FVector& WorldPoint) const { return ExoneerConstruction::NoTargetId; }

	/**
	 * SERVER. Invest WeldPoints of work into the target nearest WorldPoint,
	 * consuming stage materials from SourceInventory as thresholds pass.
	 * Returns the weld points actually applied (0 if blocked, e.g. missing materials).
	 *
	 * OutTargetId receives the identity of the target that ACTUALLY took the
	 * work, which is not always the one under WorldPoint - the aim sweep is
	 * wider than a block, so an implementation may forward the weld to the
	 * nearest unfinished neighbour. It is NoTargetId whenever nothing was
	 * applied, so the caller shows no progress instead of a stale or invented one.
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Construction")
	float InvestConstruction(AActor* Builder, UInventoryComponent* SourceInventory, const FVector& WorldPoint, float WeldPoints, int32& OutTargetId);
	virtual float InvestConstruction_Implementation(AActor* Builder, UInventoryComponent* SourceInventory, const FVector& WorldPoint, float WeldPoints, int32& OutTargetId) { OutTargetId = ExoneerConstruction::NoTargetId; return 0.f; }

	/**
	 * SERVER. Reverse construction at the target nearest WorldPoint, refunding
	 * invested materials into RefundInventory (full refund before Complete,
	 * 50% after). Returns the wreck points actually applied.
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Construction")
	float DeconstructAt(AActor* Builder, UInventoryComponent* RefundInventory, const FVector& WorldPoint, float WreckPoints);
	virtual float DeconstructAt_Implementation(AActor* Builder, UInventoryComponent* RefundInventory, const FVector& WorldPoint, float WreckPoints) { return 0.f; }
};
