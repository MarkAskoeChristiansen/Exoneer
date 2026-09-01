// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Components/InventoryComponent.h"   // FInventoryStack
#include "ExoneerTypes.h"
#include "ConstructionComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnConstructionPhaseChanged, EConstructionPhase, NewPhase);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnConstructionProgress, float, TotalProgress01);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnConstructionComplete);

/**
 * Ghost-then-invest construction state machine for a base piece.
 * (Vehicle blocks keep the equivalent state inside their replicated block
 * records; both are driven through IConstructible on the owning actor.)
 *
 * Server-only mutations; phase/progress replicate for visuals and HUD.
 * While Phase != Complete the owner must treat itself as inert: no support
 * contribution, no machine function, overlap-only collision.
 */
UCLASS(ClassGroup = (Exoneer), meta = (BlueprintSpawnableComponent))
class EXONEER_API UConstructionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UConstructionComponent();

	UPROPERTY(BlueprintAssignable) FOnConstructionPhaseChanged OnPhaseChanged;
	UPROPERTY(BlueprintAssignable) FOnConstructionProgress OnProgress;
	UPROPERTY(BlueprintAssignable) FOnConstructionComplete OnComplete;

	/** SERVER. Copy the stage plan from the definition and reset to Ghost. */
	void InitializeStages(const TArray<FConstructionCost>& InStages);

	/**
	 * SERVER. Apply WeldPoints of work. Materials are consumed from Source in
	 * proportional chunks as stage progress passes their thresholds; if the
	 * next required chunk is missing, progress stops at that threshold.
	 * Returns weld points actually applied.
	 */
	float InvestWork(UInventoryComponent* Source, float WeldPoints);

	/**
	 * SERVER. Reverse progress by WreckPoints, refunding invested materials to
	 * RefundTarget (RefundFractionComplete applies once Complete). When
	 * progress returns to 0 in Ghost phase the owner should destroy itself
	 * (listen to OnFullyDeconstructed via GetTotalProgress01() == 0).
	 * Returns wreck points actually applied.
	 */
	float DeconstructWork(UInventoryComponent* RefundTarget, float WreckPoints);

	/**
	 * SERVER. Restore a saved construction state (save/load path). Sets the
	 * replicated members directly and fires the phase/progress delegates so
	 * the owning piece and its structure react as they would to normal welding.
	 */
	void RestoreState(EConstructionPhase InPhase, int32 InStageIndex, float InStageProgress01);

	/** SERVER. Restore the invested-materials ledger (save/load path). */
	void RestoreInvested(const TArray<FInventoryEntry>& InInvested);

	UFUNCTION(BlueprintPure, Category = "Construction") EConstructionPhase GetPhase() const { return Phase; }
	UFUNCTION(BlueprintPure, Category = "Construction") bool IsComplete() const { return Phase == EConstructionPhase::Complete; }
	UFUNCTION(BlueprintPure, Category = "Construction") float GetTotalProgress01() const;
	UFUNCTION(BlueprintPure, Category = "Construction") int32 GetStageIndex() const { return StageIndex; }
	UFUNCTION(BlueprintPure, Category = "Construction") float GetStageProgress01() const { return StageProgress01; }
	UFUNCTION(BlueprintPure, Category = "Construction") const TArray<FInventoryStack>& GetInvestedMaterials() const { return Invested; }

	/** Refund fraction applied when deconstructing a Complete target. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Construction", meta = (ClampMin = "0", ClampMax = "1"))
	float RefundFractionComplete = 0.5f;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	/** Stage plan, copied from the definition on the server and replicated once. */
	UPROPERTY(Replicated)
	TArray<FConstructionCost> Stages;

	UPROPERTY(ReplicatedUsing = OnRep_Phase)
	EConstructionPhase Phase = EConstructionPhase::Ghost;

	UPROPERTY(ReplicatedUsing = OnRep_Progress)
	int32 StageIndex = 0;

	UPROPERTY(ReplicatedUsing = OnRep_Progress)
	float StageProgress01 = 0.f;

	/** Materials invested so far (for refunds). */
	UPROPERTY(Replicated)
	TArray<FInventoryStack> Invested;

	UFUNCTION() void OnRep_Phase();
	UFUNCTION() void OnRep_Progress();

	void SetPhase(EConstructionPhase NewPhase);

	/**
	 * SERVER-ONLY, not replicated. Set when deconstruction starts on a Complete
	 * target so RefundFractionComplete holds for the whole reversal (the first
	 * wreck tick downgrades the phase; without this flag every later tick
	 * would refund at 100%). Cleared when construction completes again.
	 */
	bool bDeconstructPenalty = false;
};
