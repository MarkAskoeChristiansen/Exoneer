// Copyright Exoneer contributors.
#include "Building/BlockGridActor.h"
#include "Building/BuildableBlock.h"
#include "Data/BlockDefinitionDataAsset.h"
#include "Components/PowerNetworkComponent.h"
#include "Components/PowerComponent.h"

ABlockGridActor::ABlockGridActor()
{
	PrimaryActorTick.bCanEverTick = false;
	PowerNetwork = CreateDefaultSubobject<UPowerNetworkComponent>(TEXT("PowerNetwork"));
}

FIntVector ABlockGridActor::WorldToCell(const FVector& WorldLocation) const
{
	const FVector Local = GetActorTransform().InverseTransformPosition(WorldLocation);
	return FIntVector(
		FMath::RoundToInt(Local.X / CellSize),
		FMath::RoundToInt(Local.Y / CellSize),
		FMath::RoundToInt(Local.Z / CellSize));
}

FVector ABlockGridActor::CellToWorld(const FIntVector& Cell) const
{
	const FVector Local(Cell.X * CellSize, Cell.Y * CellSize, Cell.Z * CellSize);
	return GetActorTransform().TransformPosition(Local);
}

FTransform ABlockGridActor::CellToWorldTransform(const FIntVector& Cell, int32 RotationStep) const
{
	const FQuat LocalQuat(FRotator(0.f, 90.f * (RotationStep & 3), 0.f));
	FTransform Local(LocalQuat, FVector(Cell.X * CellSize, Cell.Y * CellSize, Cell.Z * CellSize));
	return Local * GetActorTransform();
}

bool ABlockGridActor::CellsOccupied(const FIntVector& Origin, const FIntVector& Size) const
{
	for (int32 x = 0; x < Size.X; ++x)
		for (int32 y = 0; y < Size.Y; ++y)
			for (int32 z = 0; z < Size.Z; ++z)
			{
				const FIntVector C(Origin.X + x, Origin.Y + y, Origin.Z + z);
				if (Blocks.Contains(C)) return true;
			}
	return false;
}

bool ABlockGridActor::CanPlaceBlock(UBlockDefinitionDataAsset* Def, const FIntVector& Cell, int32 RotationStep, EBuildPlacementError& OutError) const
{
	OutError = EBuildPlacementError::None;
	if (!Def) { OutError = EBuildPlacementError::Unknown; return false; }
	if (bIsVehicleGrid && !Def->bAllowedOnVehicle) { OutError = EBuildPlacementError::InvalidGrid; return false; }
	if (!bIsVehicleGrid && !Def->bAllowedOnBase) { OutError = EBuildPlacementError::InvalidGrid; return false; }

	FIntVector Size = Def->GridSize;
	if (Size.X <= 0) Size.X = 1;
	if (Size.Y <= 0) Size.Y = 1;
	if (Size.Z <= 0) Size.Z = 1;
	if (CellsOccupied(Cell, Size)) { OutError = EBuildPlacementError::Overlap; return false; }

	if (Def->bRequiresSupport && !bIsVehicleGrid)
	{
		// Allow ground level (Z=0) or having a neighbor directly below.
		const bool bOnGround = (Cell.Z <= 0);
		const bool bHasSupport = Blocks.Contains(FIntVector(Cell.X, Cell.Y, Cell.Z - 1));
		if (!bOnGround && !bHasSupport) { OutError = EBuildPlacementError::NoSupport; return false; }
	}
	return true;
}

ABuildableBlock* ABlockGridActor::PlaceBlock(UBlockDefinitionDataAsset* Def, const FIntVector& Cell, int32 RotationStep)
{
	EBuildPlacementError Err;
	if (!CanPlaceBlock(Def, Cell, RotationStep, Err)) return nullptr;

	TSubclassOf<ABuildableBlock> Cls = Def->BlockActorClass ? Def->BlockActorClass : ABuildableBlock::StaticClass();
	const FTransform Xf = CellToWorldTransform(Cell, RotationStep);
	FActorSpawnParameters Sp;
	Sp.Owner = this;
	Sp.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ABuildableBlock* B = GetWorld()->SpawnActor<ABuildableBlock>(Cls, Xf, Sp);
	if (!B) return nullptr;

	B->Initialize(this, Def, Cell, RotationStep);

	// Mark every occupied cell.
	FIntVector Size = Def->GridSize;
	if (Size.X <= 0) Size.X = 1;
	if (Size.Y <= 0) Size.Y = 1;
	if (Size.Z <= 0) Size.Z = 1;
	for (int32 x = 0; x < Size.X; ++x)
		for (int32 y = 0; y < Size.Y; ++y)
			for (int32 z = 0; z < Size.Z; ++z)
			{
				Blocks.Add(FIntVector(Cell.X + x, Cell.Y + y, Cell.Z + z), B);
			}

	// Register any power component the block carries.
	if (UPowerComponent* P = B->FindComponentByClass<UPowerComponent>())
	{
		PowerNetwork->Register(P);
	}

	IBuildable::Execute_OnPlaced(B, Cell);
	return B;
}

bool ABlockGridActor::RemoveBlockAt(const FIntVector& Cell)
{
	ABuildableBlock* B = nullptr;
	if (!Blocks.RemoveAndCopyValue(Cell, B) || !B) return false;

	// Remove all duplicate references to multi-cell blocks.
	TArray<FIntVector> Keys;
	for (const auto& Kv : Blocks)
	{
		if (Kv.Value == B) Keys.Add(Kv.Key);
	}
	for (const FIntVector& K : Keys) Blocks.Remove(K);

	if (UPowerComponent* P = B->FindComponentByClass<UPowerComponent>())
	{
		PowerNetwork->Unregister(P);
	}
	IBuildable::Execute_OnRemoved(B);
	B->Destroy();
	return true;
}

ABuildableBlock* ABlockGridActor::GetBlockAt(const FIntVector& Cell) const
{
	ABuildableBlock* const* Found = Blocks.Find(Cell);
	return Found ? *Found : nullptr;
}

float ABlockGridActor::GetTotalMass() const
{
	float Total = 0.f;
	TSet<ABuildableBlock*> Counted;
	for (const auto& Kv : Blocks)
	{
		if (Counted.Contains(Kv.Value)) continue;
		Counted.Add(Kv.Value);
		if (Kv.Value && Kv.Value->Definition) Total += Kv.Value->Definition->Mass;
	}
	return Total;
}
