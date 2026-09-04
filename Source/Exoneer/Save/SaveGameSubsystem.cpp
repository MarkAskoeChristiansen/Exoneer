// Copyright Exoneer contributors.
#include "Save/SaveGameSubsystem.h"
#include "Save/ExoneerSaveGame.h"
#include "Building/BasePiece.h"
#include "Building/BaseStructure.h"
#include "Machines/MachinePiece.h"
#include "Vehicles/VehicleConstruct.h"
#include "Player/PlayerSurvivalCharacter.h"
#include "World/PlanetEnvironmentManager.h"
#include "World/ExoneerGameState.h"
#include "Data/PieceDefinitionDataAsset.h"
#include "Data/VehicleBlockDefinitionDataAsset.h"
#include "Data/ItemDefinitionDataAsset.h"
#include "Components/InventoryComponent.h"
#include "Components/ConstructionComponent.h"
#include "Components/PowerComponent.h"
#include "Components/SurvivalStatsComponent.h"
#include "Components/HealthComponent.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/UnrealType.h"
#include "EngineUtils.h"
#include "Exoneer.h"


bool USaveGameSubsystem::HasSave(const FString& SlotName) const
{
	return UGameplayStatics::DoesSaveGameExist(SlotName, 0);
}

bool USaveGameSubsystem::EnsureServer(const TCHAR* Operation) const
{
	const UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_Client)
	{
		UE_LOG(LogExoneer, Warning, TEXT("SaveGameSubsystem: %s is server-only and was ignored on this machine."), Operation);
		return false;
	}
	return true;
}

bool USaveGameSubsystem::SaveToSlot(const FString& SlotName)
{
	if (!EnsureServer(TEXT("SaveToSlot"))) return false;

	UExoneerSaveGame* Save = GatherWorldState();
	if (!Save) return false;
	Save->SlotName = SlotName;
	return UGameplayStatics::SaveGameToSlot(Save, SlotName, 0);
}

bool USaveGameSubsystem::LoadFromSlot(const FString& SlotName)
{
	if (!EnsureServer(TEXT("LoadFromSlot"))) return false;
	if (!HasSave(SlotName)) return false;

	USaveGame* Loaded = UGameplayStatics::LoadGameFromSlot(SlotName, 0);
	UExoneerSaveGame* Save = Cast<UExoneerSaveGame>(Loaded);
	if (!Save) return false;
	ApplyWorldState(Save);
	return true;
}

// ---------------------------------------------------------------------------
// Gather
// ---------------------------------------------------------------------------

UExoneerSaveGame* USaveGameSubsystem::GatherWorldState() const
{
	UWorld* World = GetWorld();
	if (!World) return nullptr;

	UExoneerSaveGame* Save = Cast<UExoneerSaveGame>(UGameplayStatics::CreateSaveGameObject(UExoneerSaveGame::StaticClass()));
	if (!Save) return nullptr;

	// Player.
	if (APlayerSurvivalCharacter* P = Cast<APlayerSurvivalCharacter>(UGameplayStatics::GetPlayerPawn(World, 0)))
	{
		Save->PlayerTransform = P->GetActorTransform();
		if (P->Inventory)
		{
			Save->PlayerInventory = P->Inventory->GetEntries();
		}
		if (P->HealthC)   Save->PlayerHealth     = P->HealthC->Health;
		if (P->Survival)
		{
			Save->PlayerOxygen      = P->Survival->Oxygen;
			Save->PlayerSuitPower   = P->Survival->SuitPower;
			Save->PlayerBodyTempC   = P->Survival->GetBodyTemperature();
			Save->PlayerSuitCondition = P->Survival->SuitCondition;
		}
	}

	// Environment.
	for (TActorIterator<APlanetEnvironmentManager> It(World); It; ++It)
	{
		Save->TimeOfDay = It->TimeOfDay01;
		break;
	}

	// Base structures, piece by piece.
	for (TActorIterator<ABaseStructure> It(World); It; ++It)
	{
		FSavedStructure Structure;
		for (ABasePiece* Piece : It->Pieces)
		{
			if (!IsValid(Piece) || !Piece->Def || !Piece->Construction) continue;

			FSavedBasePiece Rec;
			Rec.PieceId = Piece->Def->PieceId;
			Rec.WorldTransform = Piece->GetActorTransform();
			Rec.Phase = static_cast<uint8>(Piece->Construction->GetPhase());
			Rec.StageIndex = Piece->Construction->GetStageIndex();
			Rec.StageProgress01 = Piece->Construction->GetStageProgress01();
			Rec.Health = Piece->Health;
			Rec.Condition = Piece->Condition;

			// Invested ledger, so deconstruction still refunds after a load.
			for (const FInventoryStack& Stack : Piece->Construction->GetInvestedMaterials())
			{
				FInventoryEntry Entry;
				Entry.Item = Stack.Item;
				Entry.Count = Stack.Count;
				Rec.InvestedMaterials.Add(Entry);
			}

			// Internal inventory/energy exist only on machines.
			if (const AMachinePiece* Machine = Cast<AMachinePiece>(Piece))
			{
				if (Machine->Inventory) Rec.Inventory = Machine->Inventory->GetEntries();
				if (Machine->Power)     Rec.StoredEnergy = Machine->Power->StoredEnergy;
			}
			Structure.Pieces.Add(MoveTemp(Rec));
		}
		if (Structure.Pieces.Num() > 0)
		{
			Save->Structures.Add(MoveTemp(Structure));
		}
	}

	// Vehicle constructs, record by record.
	for (TActorIterator<AVehicleConstruct> It(World); It; ++It)
	{
		FSavedVehicle Vehicle;
		Vehicle.Transform = It->GetActorTransform();
		for (const FVehicleBlockRecord& Block : It->GetBlocks())
		{
			if (!Block.Def) continue;

			FSavedVehicleBlock Rec;
			Rec.BlockId = Block.Def->BlockId;
			Rec.Origin = Block.Origin;
			Rec.Orientation = Block.Orientation;
			Rec.StageIndex = Block.StageIndex;
			Rec.BuildProgress01 = Block.StageProgress01;
			Rec.Phase = static_cast<uint8>(Block.Phase);
			Rec.Health = Block.Health;
			Rec.StateScalar = Block.StateScalar;
			Rec.Condition = Block.Condition;
			if (Block.Def->bIsWheel)
			{
				It->GetWheelPersistentState(Block.BlockInstanceId, Rec.TirePressureKPa, Rec.SteerTrimDeg);
			}
			Vehicle.Blocks.Add(Rec);
		}
		if (Vehicle.Blocks.Num() > 0)
		{
			Save->Vehicles.Add(MoveTemp(Vehicle));
		}
	}

	if (AExoneerGameState* GS = World->GetGameState<AExoneerGameState>())
	{
		Save->Projects = GS->Projects;
		Save->Orbital = GS->Orbital;
		Save->SolIndex = GS->SolIndex;
	}

	return Save;
}

// ---------------------------------------------------------------------------
// Apply
// ---------------------------------------------------------------------------

void USaveGameSubsystem::ApplyWorldState(UExoneerSaveGame* Save)
{
	UWorld* World = GetWorld();
	if (!World || !Save) return;

	// Player.
	if (APlayerSurvivalCharacter* P = Cast<APlayerSurvivalCharacter>(UGameplayStatics::GetPlayerPawn(World, 0)))
	{
		P->SetActorTransform(Save->PlayerTransform);
		if (P->Inventory)
		{
			// Reset by removing the current snapshot, then re-adding the save.
			TArray<FInventoryEntry> Existing = P->Inventory->GetEntries();
			for (const FInventoryEntry& E : Existing)
			{
				if (UItemDefinitionDataAsset* Item = E.Item.LoadSynchronous())
				{
					P->Inventory->RemoveItem(Item, E.Count);
				}
			}
			for (const FInventoryEntry& E : Save->PlayerInventory)
			{
				if (UItemDefinitionDataAsset* Item = E.Item.LoadSynchronous())
				{
					P->Inventory->AddItem(Item, E.Count);
				}
			}
		}
		if (P->Survival)
		{
			P->Survival->Oxygen     = Save->PlayerOxygen;
			P->Survival->SuitPower  = Save->PlayerSuitPower;
			P->Survival->BodyTempC  = Save->PlayerBodyTempC;
			P->Survival->SuitCondition = Save->PlayerSuitCondition;
		}
		if (P->HealthC)
		{
			P->HealthC->Health = FMath::Clamp(Save->PlayerHealth, 0.f, P->HealthC->MaxHealth);
			P->HealthC->OnHealthChanged.Broadcast(P->HealthC->Health, P->HealthC->MaxHealth);
		}
	}

	// Environment.
	for (TActorIterator<APlanetEnvironmentManager> It(World); It; ++It)
	{
		It->TimeOfDay01 = Save->TimeOfDay;
		break;
	}

	// Built world: clear, then respawn from the records.
	ClearBuiltWorld(World);
	ApplyStructures(World, Save);
	ApplyVehicles(World, Save);

	if (AExoneerGameState* GS = World->GetGameState<AExoneerGameState>())
	{
		GS->Projects = Save->Projects;
		GS->Orbital = Save->Orbital;
		GS->SolIndex = Save->SolIndex;
	}
}

void USaveGameSubsystem::ClearBuiltWorld(UWorld* World) const
{
	// Collect first: destroying while iterating a TActorIterator is fragile.
	TArray<AActor*> Doomed;
	for (TActorIterator<ABasePiece> It(World); It; ++It)           Doomed.Add(*It);
	for (TActorIterator<ABaseStructure> It(World); It; ++It)       Doomed.Add(*It);
	for (TActorIterator<AVehicleConstruct> It(World); It; ++It)    Doomed.Add(*It);

	// Pieces run their normal removal bookkeeping first, then the (now empty)
	// structures and the vehicle constructs go.
	for (AActor* Actor : Doomed)
	{
		if (IsValid(Actor))
		{
			Actor->Destroy();
		}
	}
}

UPieceDefinitionDataAsset* USaveGameSubsystem::ResolvePieceDef(FName PieceId) const
{
	if (PieceId.IsNone()) return nullptr;

	UAssetManager& AssetManager = UAssetManager::Get();
	const FPrimaryAssetId AssetId(FPrimaryAssetType(TEXT("Piece")), PieceId);
	UObject* Asset = AssetManager.GetPrimaryAssetObject(AssetId);
	if (!Asset)
	{
		// Synchronous flush: load runs behind a blocking load screen, so
		// stalling the game thread here is acceptable.
		TSharedPtr<FStreamableHandle> Handle = AssetManager.LoadPrimaryAsset(AssetId);
		if (Handle.IsValid())
		{
			Handle->WaitUntilComplete();
		}
		Asset = AssetManager.GetPrimaryAssetObject(AssetId);
	}
	return Cast<UPieceDefinitionDataAsset>(Asset);
}

UVehicleBlockDefinitionDataAsset* USaveGameSubsystem::ResolveVehicleBlockDef(FName BlockId) const
{
	if (BlockId.IsNone()) return nullptr;

	UAssetManager& AssetManager = UAssetManager::Get();
	const FPrimaryAssetId AssetId(FPrimaryAssetType(TEXT("VehicleBlock")), BlockId);
	UObject* Asset = AssetManager.GetPrimaryAssetObject(AssetId);
	if (!Asset)
	{
		// Synchronous flush; see ResolvePieceDef.
		TSharedPtr<FStreamableHandle> Handle = AssetManager.LoadPrimaryAsset(AssetId);
		if (Handle.IsValid())
		{
			Handle->WaitUntilComplete();
		}
		Asset = AssetManager.GetPrimaryAssetObject(AssetId);
	}
	return Cast<UVehicleBlockDefinitionDataAsset>(Asset);
}

void USaveGameSubsystem::ApplyStructures(UWorld* World, const UExoneerSaveGame* Save) const
{
	for (const FSavedStructure& SavedStructure : Save->Structures)
	{
		TArray<ABasePiece*> Spawned;
		TArray<const FSavedBasePiece*> Pending;

		// Pass 1: grounded/parentless pieces at their exact saved transforms.
		// Groundable defs are the ones that may exist without a socket parent.
		for (const FSavedBasePiece& Rec : SavedStructure.Pieces)
		{
			UPieceDefinitionDataAsset* Def = ResolvePieceDef(Rec.PieceId);
			if (!Def)
			{
				UE_LOG(LogExoneer, Warning, TEXT("Load: unknown piece id '%s'; dropped."), *Rec.PieceId.ToString());
				continue;
			}
			if (Def->bGroundable)
			{
				EBuildPlacementError Error = EBuildPlacementError::None;
				if (ABasePiece* Piece = ABaseStructure::PlaceGroundedGhost(World, Def, Rec.WorldTransform, Error))
				{
					RestorePieceState(Piece, Rec);
					Spawned.Add(Piece);
				}
				else
				{
					UE_LOG(LogExoneer, Warning, TEXT("Load: grounded piece '%s' failed to respawn (error %d)."), *Rec.PieceId.ToString(), static_cast<int32>(Error));
				}
			}
			else
			{
				Pending.Add(&Rec);
			}
		}

		// Passes 2..N: re-link the rest to the nearest free compatible socket
		// within 1 cm (spec 12). Children can only attach once their parents
		// exist, so loop until a full pass makes no progress.
		bool bProgress = true;
		while (Pending.Num() > 0 && bProgress)
		{
			bProgress = false;
			for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
			{
				const FSavedBasePiece& Rec = *Pending[Index];
				UPieceDefinitionDataAsset* Def = ResolvePieceDef(Rec.PieceId);
				ABasePiece* Parent = nullptr;
				FName Socket = NAME_None;
				if (!Def || !FindReLinkSocket(Spawned, Def, Rec.WorldTransform, Parent, Socket))
				{
					continue;
				}
				ABaseStructure* Structure = Parent->OwningStructure;
				ABasePiece* Piece = Structure ? Structure->PlacePieceGhost(Def, Parent, Socket) : nullptr;
				if (Piece)
				{
					RestorePieceState(Piece, Rec);
					Spawned.Add(Piece);
					Pending.RemoveAt(Index);
					bProgress = true;
				}
			}
		}
		for (const FSavedBasePiece* Rec : Pending)
		{
			UE_LOG(LogExoneer, Warning, TEXT("Load: piece '%s' found no free compatible socket within 1 cm; dropped."), *Rec->PieceId.ToString());
		}

		// One support pass per resulting structure (grounded spawns may have
		// coalesced into one or split across several ABaseStructure actors).
		TSet<ABaseStructure*> Structures;
		for (ABasePiece* Piece : Spawned)
		{
			if (IsValid(Piece) && Piece->OwningStructure)
			{
				Structures.Add(Piece->OwningStructure);
			}
		}
		for (ABaseStructure* Structure : Structures)
		{
			Structure->RecomputeSupport();
		}
	}
}

bool USaveGameSubsystem::FindReLinkSocket(const TArray<ABasePiece*>& Spawned, const UPieceDefinitionDataAsset* Def,
	const FTransform& SavedTransform, ABasePiece*& OutParent, FName& OutSocket)
{
	// 1 uu == 1 cm: the spec 12 re-link tolerance.
	float BestDistSq = FMath::Square(1.f);
	bool bFound = false;

	for (ABasePiece* Parent : Spawned)
	{
		if (!IsValid(Parent) || !Parent->Def || !Parent->OwningStructure) continue;

		for (const FPieceSocketDef& SocketDef : Parent->Def->Sockets)
		{
			if (!SocketDef.AcceptedMounts.HasTag(Def->MountTag)) continue;
			if (Parent->OwningStructure->IsSocketOccupied(Parent, SocketDef.SocketName)) continue;

			const FVector SocketLocation = Parent->GetSocketWorldTransform(SocketDef.SocketName).GetLocation();
			const float DistSq = FVector::DistSquared(SocketLocation, SavedTransform.GetLocation());
			if (DistSq <= BestDistSq)
			{
				BestDistSq = DistSq;
				OutParent = Parent;
				OutSocket = SocketDef.SocketName;
				bFound = true;
			}
		}
	}
	return bFound;
}

void USaveGameSubsystem::RestorePieceState(ABasePiece* Piece, const FSavedBasePiece& Saved)
{
	if (!IsValid(Piece)) return;

	RestoreConstructionState(Piece->Construction, Saved.Phase, Saved.StageIndex, Saved.StageProgress01);
	if (Piece->Construction)
	{
		Piece->Construction->RestoreInvested(Saved.InvestedMaterials);
	}

	const float MaxHealth = Piece->Def ? Piece->Def->MaxHealth : Saved.Health;
	Piece->Health = FMath::Clamp(Saved.Health, 0.f, MaxHealth);
	Piece->Condition = Saved.Condition;

	if (AMachinePiece* Machine = Cast<AMachinePiece>(Piece))
	{
		if (Machine->Inventory)
		{
			// Fresh ghost spawn: the machine buffer starts empty, just refill.
			for (const FInventoryEntry& Entry : Saved.Inventory)
			{
				if (UItemDefinitionDataAsset* Item = Entry.Item.LoadSynchronous())
				{
					Machine->Inventory->AddItem(Item, Entry.Count);
				}
			}
		}
		// Condition is already assigned above, so re-applying the definition
		// stats here is what turns a saved capacity fade into the derated
		// StorageCapacity the stored joules are then clamped into. Without
		// this the pack would come back from disk at its rating.
		Machine->ApplyDefinitionStats();
		if (Machine->Power)
		{
			const float Capacity = Machine->Power->StorageCapacity;
			Machine->Power->StoredEnergy = Capacity > 0.f ? FMath::Clamp(Saved.StoredEnergy, 0.f, Capacity) : Saved.StoredEnergy;
		}
	}
}

void USaveGameSubsystem::RestoreConstructionState(UConstructionComponent* Construction, uint8 Phase, int32 StageIndex, float StageProgress01)
{
	if (!Construction) return;

	Construction->RestoreState(static_cast<EConstructionPhase>(Phase), StageIndex, StageProgress01);
}

void USaveGameSubsystem::ApplyVehicles(UWorld* World, const UExoneerSaveGame* Save) const
{
	for (const FSavedVehicle& SavedVehicle : Save->Vehicles)
	{
		if (SavedVehicle.Blocks.Num() == 0) continue;

		// Structural-first founding: the build tool refuses to found a
		// construct on a module block, and the save path must match - a wheel
		// as the founding record would sidestep that rule after the original
		// founder was deconstructed. Fall back to Blocks[0] if every block is
		// a module block.
		int32 FoundingIndex = 0;
		for (int32 Index = 0; Index < SavedVehicle.Blocks.Num(); ++Index)
		{
			UVehicleBlockDefinitionDataAsset* CandidateDef = ResolveVehicleBlockDef(SavedVehicle.Blocks[Index].BlockId);
			if (CandidateDef && !CandidateDef->ModuleClass)
			{
				FoundingIndex = Index;
				break;
			}
		}

		const FSavedVehicleBlock& First = SavedVehicle.Blocks[FoundingIndex];
		UVehicleBlockDefinitionDataAsset* FirstDef = ResolveVehicleBlockDef(First.BlockId);
		if (!FirstDef)
		{
			UE_LOG(LogExoneer, Warning, TEXT("Load: unknown vehicle block id '%s'; vehicle dropped."), *First.BlockId.ToString());
			continue;
		}

		// FoundConstruct places its first block at the grid origin WITH ITS SAVED
		// ORIENTATION, so every later adjacency/occupancy check runs against the
		// real footprint (founding at orientation 0 and patching afterwards
		// dropped blocks around rotated multi-cell founders). Re-base every
		// saved cell on the first block and shift the spawn transform to match.
		const FIntVector Rebase = First.Origin;
		FTransform SpawnTransform = SavedVehicle.Transform;
		SpawnTransform.AddToTranslation(SavedVehicle.Transform.TransformVector(FVector(Rebase) * AVehicleConstruct::CellSize));

		EBuildPlacementError Error = EBuildPlacementError::None;
		AVehicleConstruct* Construct = AVehicleConstruct::FoundConstruct(World, FirstDef, SpawnTransform, Error, First.Orientation);
		if (!Construct || Construct->GetBlockCount() == 0)
		{
			UE_LOG(LogExoneer, Warning, TEXT("Load: vehicle failed to respawn (error %d)."), static_cast<int32>(Error));
			continue;
		}

		// Byte-exact geometry restoration: a rover saved with wheels sunk in
		// soft soil must not lose those blocks to the world-overlap rejection.
		// The suspension settles the body on the first simulated frame.
		Construct->bSuppressWorldOverlapCheck = true;

		TArray<int32> PlacedIds;
		PlacedIds.Init(INDEX_NONE, SavedVehicle.Blocks.Num());
		PlacedIds[FoundingIndex] = Construct->GetBlocks()[0].BlockInstanceId;

		// Multi-pass placement: PlaceBlockGhost enforces face adjacency, and
		// the saved order does not guarantee it, so retry until stable.
		TArray<int32> PendingIdx;
		for (int32 Index = 0; Index < SavedVehicle.Blocks.Num(); ++Index)
		{
			if (Index != FoundingIndex)
			{
				PendingIdx.Add(Index);
			}
		}
		bool bProgress = true;
		while (PendingIdx.Num() > 0 && bProgress)
		{
			bProgress = false;
			for (int32 K = PendingIdx.Num() - 1; K >= 0; --K)
			{
				const int32 SavedIndex = PendingIdx[K];
				const FSavedVehicleBlock& Rec = SavedVehicle.Blocks[SavedIndex];
				UVehicleBlockDefinitionDataAsset* Def = ResolveVehicleBlockDef(Rec.BlockId);
				if (!Def)
				{
					UE_LOG(LogExoneer, Warning, TEXT("Load: unknown vehicle block id '%s'; block dropped."), *Rec.BlockId.ToString());
					PendingIdx.RemoveAt(K);
					continue;
				}
				const int32 BlockId = Construct->PlaceBlockGhost(Def, Rec.Origin - Rebase, Rec.Orientation);
				if (BlockId != INDEX_NONE)
				{
					PlacedIds[SavedIndex] = BlockId;
					PendingIdx.RemoveAt(K);
					bProgress = true;
				}
			}
		}
		for (int32 SavedIndex : PendingIdx)
		{
			UE_LOG(LogExoneer, Warning, TEXT("Load: vehicle block '%s' could not be re-placed (no adjacency); dropped."),
				*SavedVehicle.Blocks[SavedIndex].BlockId.ToString());
		}

		// Restore the record fields. Orientations were already placed correctly
		// (the founder included), so no post-hoc orientation patching remains.
		for (int32 Index = 0; Index < SavedVehicle.Blocks.Num(); ++Index)
		{
			if (PlacedIds[Index] != INDEX_NONE)
			{
				const FSavedVehicleBlock& Saved = SavedVehicle.Blocks[Index];
				RestoreVehicleBlockRecord(Construct, PlacedIds[Index], Saved, /*bRestoreOrientation*/ false);
				// Wheel persistent settings park on the construct until the
				// module spawns (modules are created later by the sync tick).
				if (Saved.TirePressureKPa > 0.f || Saved.SteerTrimDeg != 0.f)
				{
					Construct->QueueWheelStateRestore(PlacedIds[Index], Saved.TirePressureKPa, Saved.SteerTrimDeg);
				}
			}
		}
		Construct->bSuppressWorldOverlapCheck = false;
		Construct->MarkVisualsDirty();
	}
}

void USaveGameSubsystem::RestoreVehicleBlockRecord(AVehicleConstruct* Construct, int32 BlockInstanceId, const FSavedVehicleBlock& Saved, bool bRestoreOrientation)
{
	if (!IsValid(Construct)) return;

	Construct->RestoreBlockRecord(
		BlockInstanceId,
		static_cast<EConstructionPhase>(Saved.Phase),
		Saved.StageIndex,
		Saved.BuildProgress01,
		Saved.Health,
		Saved.StateScalar,
		bRestoreOrientation ? static_cast<int32>(Saved.Orientation) : -1);
	Construct->RestoreBlockCondition(BlockInstanceId, Saved.Condition);
}
