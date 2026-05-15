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
 * node and receives items in return. When integrity hits zero, the node is
 * destroyed.
 */
UCLASS(BlueprintType)
class EXONEER_API AResourceNode : public AActor, public IInteractable
{
	GENERATED_BODY()

public:
	AResourceNode();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) UStaticMeshComponent* Mesh = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Resource") TSoftObjectPtr<UItemDefinitionDataAsset> Yield;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Resource") int32 YieldPerHit = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Resource") float MaxIntegrity = 200.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Resource") float Integrity = 200.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Resource") float DamagePerYieldUnit = 50.f;

	/** Damage the node and drop items into the player's inventory. */
	UFUNCTION(BlueprintCallable, Category = "Resource")
	void MineByPlayer(float DamageAmount, UInventoryComponent* Inventory);

	virtual void BeginPlay() override;

	UFUNCTION(BlueprintPure, Category = "Resource")
	float GetIntegrityFraction() const { return MaxIntegrity > 0 ? Integrity / MaxIntegrity : 0.f; }

	virtual FText GetInteractionPrompt_Implementation() const override { return FText::FromString(TEXT("Mine")); }

protected:
	float DamageAccumulator = 0.f;
};
