// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Building/BuildableBlock.h"
#include "MachineBlock.generated.h"

class UPowerComponent;
class UInventoryComponent;
class UConveyorComponent;

/**
 * Base class for any powered, interactive block. Provides a PowerComponent,
 * an optional internal inventory, and a ConveyorComponent so the block can
 * push/pull from nearby storage.
 *
 * Specializations (refinery, fabricator, etc.) add a CraftingComponent on
 * top of this base.
 */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API AMachineBlock : public ABuildableBlock
{
	GENERATED_BODY()

public:
	AMachineBlock();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) UPowerComponent* Power = nullptr;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) UInventoryComponent* Inventory = nullptr;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) UConveyorComponent* Conveyor = nullptr;

	UFUNCTION(BlueprintNativeEvent, Category = "Machine")
	bool OpenMachineUI(APawn* User);
	virtual bool OpenMachineUI_Implementation(APawn* User) { return false; }

	virtual bool OnInteract_Implementation(AActor* Interactor) override;
	virtual void OnPlaced_Implementation(const FIntVector& Cell) override;
};
