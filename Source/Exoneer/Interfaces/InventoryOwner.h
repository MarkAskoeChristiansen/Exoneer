// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "InventoryOwner.generated.h"

UINTERFACE(BlueprintType, MinimalAPI)
class UInventoryOwner : public UInterface { GENERATED_BODY() };

class UInventoryComponent;

class EXONEER_API IInventoryOwner
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Inventory")
	UInventoryComponent* GetInventory() const;
	virtual UInventoryComponent* GetInventory_Implementation() const { return nullptr; }
};
