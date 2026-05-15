// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Interfaces/Buildable.h"
#include "Interfaces/Interactable.h"
#include "Interfaces/Damageable.h"
#include "BuildableBlock.generated.h"

class UBlockDefinitionDataAsset;
class ABlockGridActor;
class UStaticMeshComponent;

/**
 * A placed block in the world. Owns its current health, links back to its
 * BlockDefinition and grid, and supplies a damage state for visual feedback.
 */
UCLASS(BlueprintType)
class EXONEER_API ABuildableBlock : public AActor, public IBuildable, public IInteractable, public IDamageable
{
	GENERATED_BODY()

public:
	ABuildableBlock();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Block")
	UStaticMeshComponent* Mesh = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block")
	UBlockDefinitionDataAsset* Definition = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Block")
	FIntVector GridCoord = FIntVector::ZeroValue;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Block")
	int32 RotationStep = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Block")
	float Health = 100.f;

	UFUNCTION(BlueprintCallable, Category = "Block")
	void Repair(float Amount);

	UFUNCTION(BlueprintPure, Category = "Block")
	ABlockGridActor* GetOwningGrid() const { return OwningGrid; }

	UFUNCTION(BlueprintPure, Category = "Block")
	FIntVector GetGridCoord() const { return GridCoord; }

	void Initialize(ABlockGridActor* Grid, UBlockDefinitionDataAsset* InDef, FIntVector Cell, int32 InRotation);

	// IBuildable
	virtual UBlockDefinitionDataAsset* GetBlockDefinition_Implementation() const override { return Definition; }
	virtual void OnPlaced_Implementation(const FIntVector& Cell) override;
	virtual void OnRemoved_Implementation() override;

	// IInteractable
	virtual FText GetInteractionPrompt_Implementation() const override;

	// IDamageable
	virtual float ApplyExoneerDamage_Implementation(float Amount, EExoneerDamageType Type, AActor* Instigator) override;
	virtual float GetCurrentHealth_Implementation() const override { return Health; }
	virtual float GetMaxHealth_Implementation() const override;

protected:
	UPROPERTY() ABlockGridActor* OwningGrid = nullptr;
};
