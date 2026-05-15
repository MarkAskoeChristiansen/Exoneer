// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "BuildToolComponent.generated.h"

class UBlockDefinitionDataAsset;
class ABuildableBlock;
class ABlockGridActor;
class UStaticMeshComponent;
class UMaterialInterface;

UENUM(BlueprintType)
enum class EBuildPlacementError : uint8
{
	None,
	Overlap,
	NoSupport,
	OutOfRange,
	InvalidGrid,
	MissingComponents,
	Unknown
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnBuildPreviewChanged, bool, bValid, EBuildPlacementError, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnSelectedBlockChanged, UBlockDefinitionDataAsset*, NewBlock);

/**
 * Player-facing build placement system. Shows a ghost preview that snaps to
 * a grid, supports rotation, and commits a placement via TryConfirmPlacement.
 *
 * The actual ABuildableBlock is spawned by the target ABlockGridActor (the
 * world's base grid or a vehicle grid).
 */
UCLASS(ClassGroup = (Exoneer), meta = (BlueprintSpawnableComponent))
class EXONEER_API UBuildToolComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UBuildToolComponent();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Build") float PlacementRange = 600.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Build") TSoftObjectPtr<UMaterialInterface> ValidPreviewMaterial;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Build") TSoftObjectPtr<UMaterialInterface> InvalidPreviewMaterial;

	UPROPERTY(BlueprintAssignable) FOnBuildPreviewChanged OnBuildPreviewChanged;
	UPROPERTY(BlueprintAssignable) FOnSelectedBlockChanged OnSelectedBlockChanged;

	UFUNCTION(BlueprintCallable, Category = "Build") void SetBuildModeEnabled(bool bEnabled);
	UFUNCTION(BlueprintPure, Category = "Build") bool IsBuildModeEnabled() const { return bBuildMode; }

	UFUNCTION(BlueprintCallable, Category = "Build") void SetSelectedBlock(UBlockDefinitionDataAsset* Block);
	UFUNCTION(BlueprintPure, Category = "Build") UBlockDefinitionDataAsset* GetSelectedBlock() const { return SelectedBlock; }

	UFUNCTION(BlueprintCallable, Category = "Build") void RotateBlock(int32 Steps = 1);

	UFUNCTION(BlueprintCallable, Category = "Build") bool TryConfirmPlacement();
	UFUNCTION(BlueprintCallable, Category = "Build") bool TryRemoveTargetedBlock();
	UFUNCTION(BlueprintCallable, Category = "Build") bool TryRepairTargetedBlock(float Amount);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn) override;

protected:
	UPROPERTY(VisibleInstanceOnly) bool bBuildMode = false;
	UPROPERTY(VisibleInstanceOnly) UBlockDefinitionDataAsset* SelectedBlock = nullptr;
	UPROPERTY() UStaticMeshComponent* PreviewMesh = nullptr;
	UPROPERTY() ABlockGridActor* TargetGrid = nullptr;
	UPROPERTY() FIntVector PreviewCell = FIntVector::ZeroValue;
	UPROPERTY() int32 RotationStep = 0;  // 0..3 around Z

	bool bLastPreviewValid = false;
	EBuildPlacementError LastError = EBuildPlacementError::None;

	void EnsurePreviewMesh();
	void UpdatePreview();
	bool ResolveTarget(FIntVector& OutCell, ABlockGridActor*& OutGrid, EBuildPlacementError& OutError);
};
