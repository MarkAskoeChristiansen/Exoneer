// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "ExoneerTypes.h"
#include "ProjectSubsystem.generated.h"

class UProjectDefinitionDataAsset;
class AExoneerGameState;
class APlayerSurvivalCharacter;

/**
 * Server-authoritative optional projects. Evaluates physical criteria at 1 Hz.
 * Clients read AExoneerGameState::Projects.
 */
UCLASS()
class EXONEER_API UProjectSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UProjectSubsystem, STATGROUP_Tickables); }
	virtual bool IsTickable() const override { return true; }
	virtual bool IsTickableInEditor() const override { return false; }

	UFUNCTION(BlueprintCallable, Category = "Project")
	bool AcceptProject(FName ProjectId);

	UFUNCTION(BlueprintCallable, Category = "Project")
	bool AbandonProject(FName ProjectId);

	UFUNCTION(BlueprintPure, Category = "Project")
	UProjectDefinitionDataAsset* FindDef(FName ProjectId) const;

protected:
	float EvalAccumulator = 0.f;
	int32 LastSolIndex = 0;
	float LastTimeOfDay01 = 0.f;

	AExoneerGameState* GetGS() const;
	void EnsureCatalog();
	void EvaluateActive();
	bool EvaluateProject(const UProjectDefinitionDataAsset* Def, FProjectRuntime& Runtime, FString& OutFail) const;
	bool CheckCriterion(EProjectCriterionType Type, float Target, const FProjectRuntime& Runtime, FString& OutFail) const;
};
