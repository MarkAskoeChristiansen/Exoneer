// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ConveyorComponent.generated.h"

class UInventoryComponent;
class UItemDefinitionDataAsset;

/**
 * Lets a machine pull items from, or push items to, connected inventories.
 *
 * In the prototype, "connected" simply means "any inventory within
 * ConnectionRange of the owning actor". A future expansion would walk the
 * socket graph of placed conveyor pieces for true network logic.
 *
 * SERVER-ONLY: both transfer calls mutate inventories and therefore run only
 * on authority (they log and return 0 on clients).
 */
UCLASS(ClassGroup = (Exoneer), meta = (BlueprintSpawnableComponent))
class EXONEER_API UConveyorComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UConveyorComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Conveyor") float ConnectionRange = 600.f;

	/** SERVER. Pull up to Count of Item from any connected inventory into Destination. Returns pulled. */
	UFUNCTION(BlueprintCallable, Category = "Conveyor")
	int32 PullInto(UInventoryComponent* Destination, UItemDefinitionDataAsset* Item, int32 Count);

	/** SERVER. Push up to Count of Item from Source into any connected inventory. Returns pushed. */
	UFUNCTION(BlueprintCallable, Category = "Conveyor")
	int32 PushFrom(UInventoryComponent* Source, UItemDefinitionDataAsset* Item, int32 Count);

protected:
	void GatherNearbyInventories(TArray<UInventoryComponent*>& OutInventories) const;

	bool HasAuthority() const;
};
