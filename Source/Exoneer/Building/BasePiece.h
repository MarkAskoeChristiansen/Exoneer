// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "CoreGlobals.h"   // GFrameCounter, the ReportLiveLoad frame stamp default
#include "GameFramework/Actor.h"
#include "Interfaces/Interactable.h"
#include "Interfaces/Constructible.h"
#include "Interfaces/Damageable.h"
#include "ExoneerTypes.h"
#include "BasePiece.generated.h"

class UPieceDefinitionDataAsset;
class UConstructionComponent;
class ABaseStructure;
class UStaticMeshComponent;
class UInventoryComponent;

/**
 * One placed architectural piece (foundation, wall, floor, ramp, roof, beam)
 * or - via AMachinePiece - a deployable machine.
 *
 * Spawned by ABaseStructure as a GHOST, finished by welding. The parent
 * piece/socket replicate so clients can mirror socket occupancy for build
 * previews without asking the server.
 */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API ABasePiece : public AActor, public IInteractable, public IConstructible, public IDamageable
{
	GENERATED_BODY()

public:
	ABasePiece();

	/**
	 * Scene root at the MOUNT POINT (socket / ground hit). The mesh hangs off
	 * it as a child so visual alignment offsets never move the actor itself.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Piece")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Piece")
	TObjectPtr<UStaticMeshComponent> Mesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Piece")
	TObjectPtr<UConstructionComponent> Construction;

	UPROPERTY(ReplicatedUsing = OnRep_Def, VisibleAnywhere, BlueprintReadOnly, Category = "Piece")
	TObjectPtr<UPieceDefinitionDataAsset> Def;

	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Piece")
	TObjectPtr<ABaseStructure> OwningStructure;

	/** Piece this one snapped into, and which of its sockets. Null when grounded. */
	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Piece")
	TObjectPtr<ABasePiece> ParentPiece;

	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Piece")
	FName ParentSocket;

	UPROPERTY(ReplicatedUsing = OnRep_Health, VisibleAnywhere, BlueprintReadOnly, Category = "Piece")
	float Health = 1.f;

	/** Remaining support units after the solver pass; <= 0 means collapsing. */
	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Piece")
	int32 SupportValue = 0;

	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Piece")
	FPartCondition Condition;

	/**
	 * Live wheel load (N) at the last 5 Hz load pass, published for the
	 * focused-piece panel. Zeroed when the pass disarms, so the wrist never
	 * shows the load of a rover that drove off an hour ago.
	 */
	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Piece")
	float LastLoadN = 0.f;

	/**
	 * SERVER, transient. Live-load accumulators drained by the owning
	 * structure's 5 Hz load pass: PendingLoadN is the sum of every wheel
	 * report since the last read, PendingLoadFrames the number of distinct
	 * FRAMES those reports arrived in, so the mean is the load the piece
	 * actually carries with all wheels on it at once.
	 */
	float PendingLoadN = 0.f;
	int32 PendingLoadFrames = 0;

	/**
	 * SERVER, transient. Frame stamp of the last report, so several wheels
	 * reporting inside one frame count as ONE frame.
	 */
	uint64 LastLoadReportFrame = TNumericLimits<uint64>::Max();

	/**
	 * SERVER, transient. Permanent set earned but not yet published: the
	 * replicated reading moves in 0.5 mm steps, so a deck barely over its
	 * rating does not send an update every load pass.
	 */
	float PendingDeflectionMm = 0.f;

	/**
	 * SERVER, transient. Set by the load pass on a gross overload. The support
	 * solver then gives the piece no seed, no relaxed support and no grounded
	 * exemption, so the existing collapse batch takes it.
	 */
	bool bLoadOverloaded = false;

	/** SERVER. Called by ABaseStructure right after spawning the ghost. */
	void InitializeGhost(ABaseStructure* Structure, UPieceDefinitionDataAsset* InDef, ABasePiece* InParent, FName InParentSocket);

	/** World transform of one of this piece's definition sockets. */
	UFUNCTION(BlueprintPure, Category = "Piece")
	FTransform GetSocketWorldTransform(FName SocketName) const;

	UFUNCTION(BlueprintPure, Category = "Piece")
	bool IsFunctional() const;   // Complete && Health > 0

	/** SERVER. Clear solar/radio dust. */
	bool WipeDust();

	/**
	 * True when this piece carries a reading with nothing left to spend, so
	 * spending a fabricated spare on it is legal. At alpha the only piece
	 * class with a spare is the battery bank, whose terminal reading is
	 * capacity fade at the floor. A sagged deck is NOT terminal in this
	 * sense: its verb is rebuild, not replace.
	 */
	UFUNCTION(BlueprintPure, Category = "Piece")
	bool IsConditionTerminal() const;

	/**
	 * SERVER. Spend one authored spare from Source and fit it: the reading
	 * goes back to nominal, the piece keeps its placement, its structure and
	 * its health. Refused unless the piece is Complete, its definition names a
	 * spare, the reading is terminal, and the spare is in the pack.
	 */
	bool ReplacePart(UInventoryComponent* Source);

	/**
	 * SERVER. Fresh part: every reading back to the definition's nominal
	 * values, and every server-side accumulator behind them cleared. Machines
	 * extend this to re-apply the stats their readings derate.
	 */
	virtual void ResetConditionToNominal();

	/**
	 * SERVER. One wheel's normal load (N) pressing on this piece this frame.
	 * Ghosts and half-welded pieces refuse it: a planning marker carries
	 * nothing and must never take a permanent set. FrameStamp defaults to the
	 * engine frame counter and exists so a test can drive whole frames.
	 */
	void ReportLiveLoad(float LoadN, uint64 FrameStamp = GFrameCounter);

	/** Permanent set past the definition's terminal reading: stands, carries nothing, rebuild it. */
	UFUNCTION(BlueprintPure, Category = "Piece")
	bool IsLoadCondemned() const;

	/** SERVER. Storm exposure spends surface opacity. */
	void ApplyWeatherWear(float StormIntensity, float DtSeconds);

	/** BP hook fired when construction completes (spawn FX, enable lights...). */
	UFUNCTION(BlueprintImplementableEvent, Category = "Piece")
	void OnConstructionCompletedBP();

	// IInteractable
	virtual FText GetInteractionPrompt_Implementation() const override;
	virtual FGameplayTagContainer GetInteractionTags_Implementation() const override;

	// IConstructible (forwards to the Construction component)
	virtual EConstructionPhase GetConstructionPhaseAt_Implementation(const FVector& WorldPoint) const override;
	virtual float GetConstructionProgressAt_Implementation(const FVector& WorldPoint) const override;
	virtual float GetConstructionProgressForTarget_Implementation(int32 TargetId) const override;
	virtual int32 GetConstructionTargetIdAt_Implementation(const FVector& WorldPoint) const override;
	virtual float InvestConstruction_Implementation(AActor* Builder, UInventoryComponent* SourceInventory, const FVector& WorldPoint, float WeldPoints, int32& OutTargetId) override;
	virtual float DeconstructAt_Implementation(AActor* Builder, UInventoryComponent* RefundInventory, const FVector& WorldPoint, float WreckPoints) override;

	// IDamageable
	virtual float ApplyExoneerDamage_Implementation(float Amount, EExoneerDamageType Type, AActor* Instigator) override;
	virtual float GetCurrentHealth_Implementation() const override { return Health; }
	virtual float GetMaxHealth_Implementation() const override;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	virtual void BeginPlay() override;

	UFUNCTION() void OnRep_Def();
	UFUNCTION() void OnRep_Health();

	/** Apply mesh/collision/material for the current phase (all machines). */
	void RefreshVisualState();

	/** Bound to Construction->OnPhaseChanged (server + client). */
	UFUNCTION() void HandlePhaseChanged(EConstructionPhase NewPhase);

};
