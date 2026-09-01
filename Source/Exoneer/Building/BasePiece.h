// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
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

	/** SERVER. Called by ABaseStructure right after spawning the ghost. */
	void InitializeGhost(ABaseStructure* Structure, UPieceDefinitionDataAsset* InDef, ABasePiece* InParent, FName InParentSocket);

	/** World transform of one of this piece's definition sockets. */
	UFUNCTION(BlueprintPure, Category = "Piece")
	FTransform GetSocketWorldTransform(FName SocketName) const;

	UFUNCTION(BlueprintPure, Category = "Piece")
	bool IsFunctional() const;   // Complete && Health > 0

	/** SERVER. Restore up to Amount health (weld-to-repair). Returns health restored. */
	float RepairHealth(float Amount);

	/** BP hook fired when construction completes (spawn FX, enable lights...). */
	UFUNCTION(BlueprintImplementableEvent, Category = "Piece")
	void OnConstructionCompletedBP();

	// IInteractable
	virtual FText GetInteractionPrompt_Implementation() const override;
	virtual FGameplayTagContainer GetInteractionTags_Implementation() const override;

	// IConstructible (forwards to the Construction component)
	virtual EConstructionPhase GetConstructionPhaseAt_Implementation(const FVector& WorldPoint) const override;
	virtual float GetConstructionProgressAt_Implementation(const FVector& WorldPoint) const override;
	virtual float InvestConstruction_Implementation(AActor* Builder, UInventoryComponent* SourceInventory, const FVector& WorldPoint, float WeldPoints) override;
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
