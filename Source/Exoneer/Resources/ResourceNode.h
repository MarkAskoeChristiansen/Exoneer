// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Interfaces/Interactable.h"
#include "ResourceNode.generated.h"

class UItemDefinitionDataAsset;
class UInventoryComponent;
class UStaticMeshComponent;

/**
 * A mineable rock / ice / ore deposit. The player's mining tool damages the
 * node ON THE SERVER (intent arrives via UMiningToolComponent's
 * Server_MineTarget) and yield lands in the mining player's inventory.
 * Integrity replicates so every client sees the deposit shrink; at zero the
 * server fires the depletion FX hook and destroys the node (replication
 * tears it down on clients).
 */
UCLASS(BlueprintType)
class EXONEER_API AResourceNode : public AActor, public IInteractable
{
	GENERATED_BODY()

public:
	AResourceNode();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) TObjectPtr<UStaticMeshComponent> Mesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Resource") TSoftObjectPtr<UItemDefinitionDataAsset> Yield;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Resource") int32 YieldPerHit = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Resource") float MaxIntegrity = 200.f;
	UPROPERTY(ReplicatedUsing = OnRep_Integrity, VisibleAnywhere, BlueprintReadOnly, Category = "Resource") float Integrity = 200.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Resource") float DamagePerYieldUnit = 50.f;

	/** Per-instance deposit richness; multiplies every yield count. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Resource", meta = (ClampMin = "0"))
	float YieldMultiplier = 1.f;

	/** SERVER-ONLY. Damage the node and drop items into the player's inventory. */
	UFUNCTION(BlueprintCallable, Category = "Resource")
	void MineByPlayer(float DamageAmount, UInventoryComponent* Inventory);

	virtual void BeginPlay() override;

	UFUNCTION(BlueprintPure, Category = "Resource")
	float GetIntegrityFraction() const { return MaxIntegrity > 0 ? Integrity / MaxIntegrity : 0.f; }

	/** BP hook for depletion FX (dust burst, audio); fires on every machine. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Resource")
	void OnDepletedBP();

	virtual FText GetInteractionPrompt_Implementation() const override { return FText::FromString(TEXT("Mine")); }

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	float DamageAccumulator = 0.f;

	/** Spawn-time mesh scale; integrity feedback scales relative to this. */
	FVector BaseMeshScale = FVector::OneVector;

	UFUNCTION() void OnRep_Integrity();

	/** Mesh feedback shared by server mining and client replication. */
	void ApplyIntegrityVisuals();

	/** FX hook broadcast just before the server destroys the depleted node. */
	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_OnDepleted();
};
