// Copyright Exoneer contributors.
#include "Components/BuildToolComponent.h"
#include "Components/InventoryComponent.h"
#include "Data/BlockDefinitionDataAsset.h"
#include "Building/BuildableBlock.h"
#include "Building/BlockGridActor.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Exoneer.h"

UBuildToolComponent::UBuildToolComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UBuildToolComponent::SetBuildModeEnabled(bool bEnabled)
{
	bBuildMode = bEnabled;
	if (!bEnabled && PreviewMesh)
	{
		PreviewMesh->SetVisibility(false);
	}
}

void UBuildToolComponent::SetSelectedBlock(UBlockDefinitionDataAsset* Block)
{
	SelectedBlock = Block;
	OnSelectedBlockChanged.Broadcast(SelectedBlock);
	if (PreviewMesh && SelectedBlock)
	{
		if (UStaticMesh* M = SelectedBlock->PreviewMesh.LoadSynchronous())
		{
			PreviewMesh->SetStaticMesh(M);
		}
	}
}

void UBuildToolComponent::RotateBlock(int32 Steps)
{
	RotationStep = (RotationStep + Steps) & 0x3;
}

void UBuildToolComponent::EnsurePreviewMesh()
{
	if (PreviewMesh || !GetOwner()) return;
	PreviewMesh = NewObject<UStaticMeshComponent>(GetOwner());
	PreviewMesh->SetMobility(EComponentMobility::Movable);
	PreviewMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PreviewMesh->SetCastShadow(false);
	PreviewMesh->RegisterComponent();
	PreviewMesh->AttachToComponent(GetOwner()->GetRootComponent(),
		FAttachmentTransformRules::KeepWorldTransform);
	PreviewMesh->SetVisibility(false);
}

bool UBuildToolComponent::ResolveTarget(FIntVector& OutCell, ABlockGridActor*& OutGrid, EBuildPlacementError& OutError)
{
	OutError = EBuildPlacementError::None;
	AActor* Owner = GetOwner();
	if (!Owner || !SelectedBlock) { OutError = EBuildPlacementError::Unknown; return false; }

	UCameraComponent* Cam = Owner->FindComponentByClass<UCameraComponent>();
	const FVector Start = Cam ? Cam->GetComponentLocation() : Owner->GetActorLocation();
	const FVector Dir = Cam ? Cam->GetForwardVector() : Owner->GetActorForwardVector();
	const FVector End = Start + Dir * PlacementRange;

	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ExoneerBuild), false, Owner);
	const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, Params);

	// Pick the first ABlockGridActor in the world for the prototype. A real
	// implementation would let the player select a grid (e.g. by aiming at
	// an existing block on a vehicle vs. the base grid).
	OutGrid = nullptr;
	for (TActorIterator<ABlockGridActor> It(GetWorld()); It; ++It) { OutGrid = *It; break; }
	if (!OutGrid) { OutError = EBuildPlacementError::InvalidGrid; return false; }

	const FVector TargetWorld = bHit ? Hit.ImpactPoint + Hit.ImpactNormal * 1.f : End;
	OutCell = OutGrid->WorldToCell(TargetWorld);

	if (!OutGrid->CanPlaceBlock(SelectedBlock, OutCell, RotationStep, OutError))
	{
		return false;
	}
	return true;
}

void UBuildToolComponent::UpdatePreview()
{
	EnsurePreviewMesh();
	if (!PreviewMesh) return;

	if (!bBuildMode || !SelectedBlock)
	{
		PreviewMesh->SetVisibility(false);
		return;
	}

	ABlockGridActor* Grid = nullptr;
	FIntVector Cell;
	EBuildPlacementError Err;
	const bool bValid = ResolveTarget(Cell, Grid, Err);

	PreviewCell = Cell;
	TargetGrid = Grid;

	if (Grid)
	{
		const FTransform Xf = Grid->CellToWorldTransform(Cell, RotationStep);
		PreviewMesh->SetWorldTransform(Xf);
		PreviewMesh->SetVisibility(true);

		UMaterialInterface* Mat = (bValid ? ValidPreviewMaterial : InvalidPreviewMaterial).LoadSynchronous();
		if (Mat)
		{
			for (int32 i = 0; i < PreviewMesh->GetNumMaterials(); ++i)
			{
				PreviewMesh->SetMaterial(i, Mat);
			}
		}
	}
	else
	{
		PreviewMesh->SetVisibility(false);
	}

	if (bValid != bLastPreviewValid || Err != LastError)
	{
		bLastPreviewValid = bValid;
		LastError = Err;
		OnBuildPreviewChanged.Broadcast(bValid, Err);
	}
}

void UBuildToolComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn)
{
	Super::TickComponent(DeltaTime, TickType, TickFn);
	if (bBuildMode) UpdatePreview();
}

bool UBuildToolComponent::TryConfirmPlacement()
{
	if (!bBuildMode || !SelectedBlock) return false;

	ABlockGridActor* Grid = nullptr;
	FIntVector Cell;
	EBuildPlacementError Err;
	if (!ResolveTarget(Cell, Grid, Err) || !Grid) return false;

	// Consume build cost from the player's inventory.
	UInventoryComponent* Inv = GetOwner() ? GetOwner()->FindComponentByClass<UInventoryComponent>() : nullptr;
	if (Inv && SelectedBlock->BuildCost.Num() > 0)
	{
		TArray<FInventoryEntry> Required;
		Required.Reserve(SelectedBlock->BuildCost.Num());
		for (const FBlockBuildCost& C : SelectedBlock->BuildCost)
		{
			Required.Add({ C.Item, C.Count });
		}
		if (!Inv->ConsumeItems(Required))
		{
			LastError = EBuildPlacementError::MissingComponents;
			OnBuildPreviewChanged.Broadcast(false, LastError);
			return false;
		}
	}

	return Grid->PlaceBlock(SelectedBlock, Cell, RotationStep) != nullptr;
}

bool UBuildToolComponent::TryRemoveTargetedBlock()
{
	AActor* Owner = GetOwner();
	if (!Owner) return false;
	UCameraComponent* Cam = Owner->FindComponentByClass<UCameraComponent>();
	const FVector Start = Cam ? Cam->GetComponentLocation() : Owner->GetActorLocation();
	const FVector Dir = Cam ? Cam->GetForwardVector() : Owner->GetActorForwardVector();
	const FVector End = Start + Dir * PlacementRange;

	FHitResult Hit;
	FCollisionQueryParams P(SCENE_QUERY_STAT(ExoneerBuildRemove), false, Owner);
	if (GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, P))
	{
		if (ABuildableBlock* Block = Cast<ABuildableBlock>(Hit.GetActor()))
		{
			if (ABlockGridActor* G = Block->GetOwningGrid())
			{
				return G->RemoveBlockAt(Block->GetGridCoord());
			}
		}
	}
	return false;
}

bool UBuildToolComponent::TryRepairTargetedBlock(float Amount)
{
	AActor* Owner = GetOwner();
	if (!Owner) return false;
	UCameraComponent* Cam = Owner->FindComponentByClass<UCameraComponent>();
	const FVector Start = Cam ? Cam->GetComponentLocation() : Owner->GetActorLocation();
	const FVector Dir = Cam ? Cam->GetForwardVector() : Owner->GetActorForwardVector();
	const FVector End = Start + Dir * PlacementRange;

	FHitResult Hit;
	FCollisionQueryParams P(SCENE_QUERY_STAT(ExoneerBuildRepair), false, Owner);
	if (GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, P))
	{
		if (ABuildableBlock* Block = Cast<ABuildableBlock>(Hit.GetActor()))
		{
			Block->Repair(Amount);
			return true;
		}
	}
	return false;
}
