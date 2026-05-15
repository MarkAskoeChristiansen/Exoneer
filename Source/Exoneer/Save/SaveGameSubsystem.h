// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SaveGameSubsystem.generated.h"

class UExoneerSaveGame;

/**
 * High-level save/load entry point. Walks the world, gathers state from the
 * player, every block grid, and the environment manager, then serialises into
 * an UExoneerSaveGame.
 */
UCLASS(BlueprintType)
class EXONEER_API USaveGameSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "Save") bool SaveToSlot(const FString& SlotName);
	UFUNCTION(BlueprintCallable, Category = "Save") bool LoadFromSlot(const FString& SlotName);
	UFUNCTION(BlueprintCallable, Category = "Save") bool HasSave(const FString& SlotName) const;

protected:
	UExoneerSaveGame* GatherWorldState() const;
	void ApplyWorldState(UExoneerSaveGame* Save);
};
