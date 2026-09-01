// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "ExoneerTypes.h"
#include "Constructible.generated.h"

class UInventoryComponent;

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

	/** Total 0..1 progress of the construction target nearest WorldPoint. */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Construction")
	float GetConstructionProgressAt(const FVector& WorldPoint) const;
	virtual float GetConstructionProgressAt_Implementation(const FVector& WorldPoint) const { return 1.f; }

	/**
	 * SERVER. Invest WeldPoints of work into the target nearest WorldPoint,
	 * consuming stage materials from SourceInventory as thresholds pass.
	 * Returns the weld points actually applied (0 if blocked, e.g. missing materials).
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Construction")
	float InvestConstruction(AActor* Builder, UInventoryComponent* SourceInventory, const FVector& WorldPoint, float WeldPoints);
	virtual float InvestConstruction_Implementation(AActor* Builder, UInventoryComponent* SourceInventory, const FVector& WorldPoint, float WeldPoints) { return 0.f; }

	/**
	 * SERVER. Reverse construction at the target nearest WorldPoint, refunding
	 * invested materials into RefundInventory (full refund before Complete,
	 * 50% after). Returns the wreck points actually applied.
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Construction")
	float DeconstructAt(AActor* Builder, UInventoryComponent* RefundInventory, const FVector& WorldPoint, float WreckPoints);
	virtual float DeconstructAt_Implementation(AActor* Builder, UInventoryComponent* RefundInventory, const FVector& WorldPoint, float WreckPoints) { return 0.f; }
};
