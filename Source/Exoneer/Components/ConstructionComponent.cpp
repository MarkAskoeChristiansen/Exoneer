// Copyright Exoneer contributors.
#include "Components/ConstructionComponent.h"
#include "Data/ItemDefinitionDataAsset.h"
#include "GameFramework/Actor.h"
#include "Net/UnrealNetwork.h"
#include "Exoneer.h"

namespace
{
	/**
	 * When welding stalls on a missing material unit, progress parks just below
	 * the unit's threshold so the same unit is demanded again on the next call
	 * (and after a save/load round trip). Must be larger than the threshold
	 * detection epsilon divided by any realistic per-stage unit count.
	 */
	constexpr float BlockedThresholdEpsilon = 2e-4f;

	/**
	 * Units of a material entry with Count total that are due once stage
	 * progress reaches P. Unit k's threshold is k/Count; reaching the
	 * threshold exactly counts the unit as due.
	 */
	int32 UnitsDueAt(float Progress01, int32 Count)
	{
		return FMath::Clamp(FMath::FloorToInt32(Progress01 * Count + UE_KINDA_SMALL_NUMBER), 0, Count);
	}
}

UConstructionComponent::UConstructionComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UConstructionComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// The stage plan is written once right after spawn and never changes.
	DOREPLIFETIME_CONDITION(UConstructionComponent, Stages, COND_InitialOnly);
	DOREPLIFETIME(UConstructionComponent, Phase);
	DOREPLIFETIME(UConstructionComponent, StageIndex);
	DOREPLIFETIME(UConstructionComponent, StageProgress01);
	DOREPLIFETIME(UConstructionComponent, Invested);
}

void UConstructionComponent::InitializeStages(const TArray<FConstructionCost>& InStages)
{
	AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority())
	{
		UE_LOG(LogExoneer, Warning, TEXT("InitializeStages called without authority on %s"), *GetNameSafe(Owner));
		return;
	}

	Stages = InStages;
	StageIndex = 0;
	StageProgress01 = 0.f;
	Invested.Reset();
	Phase = EConstructionPhase::Ghost;

	// Broadcast unconditionally so the owner refreshes its ghost visuals even
	// when the component was already at the defaults.
	OnPhaseChanged.Broadcast(Phase);
	OnProgress.Broadcast(0.f);
}

float UConstructionComponent::InvestWork(UInventoryComponent* Source, float WeldPoints)
{
	AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority())
	{
		UE_LOG(LogExoneer, Warning, TEXT("InvestWork called without authority on %s"), *GetNameSafe(Owner));
		return 0.f;
	}
	if (Phase == EConstructionPhase::Complete || WeldPoints <= 0.f)
	{
		return 0.f;
	}

	// No investment plan authored: the first weld completes the target
	// (mirrors the vehicle block rule; an empty Stages array must never leave
	// the piece stuck as a ghost forever).
	if (Stages.Num() == 0)
	{
		StageIndex = 0;
		StageProgress01 = 1.f;
		bDeconstructPenalty = false;
		SetPhase(EConstructionPhase::Complete);
		OnComplete.Broadcast();
		OnProgress.Broadcast(1.f);
		return WeldPoints;
	}
	if (!Stages.IsValidIndex(StageIndex))
	{
		return 0.f;
	}

	// Merge one consumed unit into the invested ledger (for refunds).
	auto AddInvestedUnit = [this](UItemDefinitionDataAsset* Item)
	{
		for (FInventoryStack& Stack : Invested)
		{
			if (Stack.Item == Item)
			{
				Stack.Count++;
				return;
			}
		}
		FInventoryStack NewStack;
		NewStack.Item = Item;
		NewStack.Count = 1;
		Invested.Add(NewStack);
	};

	float Remaining = WeldPoints;
	float Applied = 0.f;
	bool bBlocked = false;

	while (Remaining > UE_KINDA_SMALL_NUMBER && Stages.IsValidIndex(StageIndex))
	{
		const FConstructionCost& Stage = Stages[StageIndex];
		const float Work = FMath::Max(Stage.WeldWork, UE_KINDA_SMALL_NUMBER);
		const float StartP = StageProgress01;
		float TargetP = FMath::Min(1.f, StartP + Remaining / Work);

		// Material units whose thresholds fall inside (StartP, TargetP]. An
		// entry with Count N consumes one unit each time progress crosses k/N.
		struct FPendingUnit
		{
			float Threshold;
			UItemDefinitionDataAsset* Item;
		};
		TArray<FPendingUnit> Pending;
		for (const FInventoryEntry& Entry : Stage.Materials)
		{
			if (Entry.Count <= 0)
			{
				continue;
			}
			UItemDefinitionDataAsset* Item = Entry.Item.LoadSynchronous();
			if (!Item)
			{
				UE_LOG(LogExoneer, Warning, TEXT("%s: stage %d has an unresolved material; treating it as free"),
					*GetNameSafe(Owner), StageIndex);
				continue;
			}
			const int32 From = UnitsDueAt(StartP, Entry.Count);
			const int32 To = UnitsDueAt(TargetP, Entry.Count);
			for (int32 Unit = From + 1; Unit <= To; ++Unit)
			{
				Pending.Add({ static_cast<float>(Unit) / static_cast<float>(Entry.Count), Item });
			}
		}
		Pending.Sort([](const FPendingUnit& A, const FPendingUnit& B) { return A.Threshold < B.Threshold; });

		for (const FPendingUnit& Unit : Pending)
		{
			if (Source && Source->RemoveItem(Unit.Item, 1) == 1)
			{
				AddInvestedUnit(Unit.Item);
				continue;
			}
			// The next required unit is missing: clamp progress at its
			// threshold and stop welding.
			TargetP = FMath::Max(StartP, Unit.Threshold - BlockedThresholdEpsilon);
			bBlocked = true;
			break;
		}

		const float StageApplied = (TargetP - StartP) * Work;
		StageProgress01 = TargetP;
		Applied += StageApplied;
		Remaining -= StageApplied;

		if (bBlocked)
		{
			break;
		}

		if (TargetP >= 1.f - UE_KINDA_SMALL_NUMBER)
		{
			if (StageIndex >= Stages.Num() - 1)
			{
				StageProgress01 = 1.f;
				bDeconstructPenalty = false; // Welded back whole: penalty resets.
				SetPhase(EConstructionPhase::Complete);
				OnComplete.Broadcast();
				break;
			}
			StageIndex++;
			StageProgress01 = 0.f;
		}
	}

	if (Applied > 0.f)
	{
		if (Phase == EConstructionPhase::Ghost)
		{
			SetPhase(EConstructionPhase::UnderConstruction);
		}
		OnProgress.Broadcast(GetTotalProgress01());
	}
	return Applied;
}

float UConstructionComponent::DeconstructWork(UInventoryComponent* RefundTarget, float WreckPoints)
{
	AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority())
	{
		UE_LOG(LogExoneer, Warning, TEXT("DeconstructWork called without authority on %s"), *GetNameSafe(Owner));
		return 0.f;
	}
	if (WreckPoints <= 0.f || !Stages.IsValidIndex(StageIndex))
	{
		return 0.f;
	}

	// A target that was ever Complete refunds at the penalty fraction for the
	// WHOLE reversal; the sticky flag survives the phase downgrade so later
	// wreck ticks cannot escape it. Welding back to Complete clears the flag.
	const bool bWasComplete = Phase == EConstructionPhase::Complete;
	if (bWasComplete)
	{
		bDeconstructPenalty = true;
	}
	const float RefundFraction = bDeconstructPenalty ? RefundFractionComplete : 1.f;

	float Remaining = WreckPoints;
	float Applied = 0.f;
	TMap<UItemDefinitionDataAsset*, int32> UncrossedUnits;

	while (Remaining > UE_KINDA_SMALL_NUMBER)
	{
		if (StageProgress01 <= 0.f)
		{
			if (StageIndex == 0)
			{
				break;
			}
			// Step down into the previous, fully welded stage.
			StageIndex--;
			StageProgress01 = 1.f;
			continue;
		}

		const FConstructionCost& Stage = Stages[StageIndex];
		const float Work = FMath::Max(Stage.WeldWork, UE_KINDA_SMALL_NUMBER);
		const float StartP = StageProgress01;
		const float NewP = FMath::Max(0.f, StartP - Remaining / Work);

		// Material thresholds un-crossed inside (NewP, StartP] fall back out.
		for (const FInventoryEntry& Entry : Stage.Materials)
		{
			if (Entry.Count <= 0)
			{
				continue;
			}
			UItemDefinitionDataAsset* Item = Entry.Item.LoadSynchronous();
			if (!Item)
			{
				continue;
			}
			const int32 Units = UnitsDueAt(StartP, Entry.Count) - UnitsDueAt(NewP, Entry.Count);
			if (Units > 0)
			{
				UncrossedUnits.FindOrAdd(Item) += Units;
			}
		}

		const float StageApplied = (StartP - NewP) * Work;
		StageProgress01 = NewP;
		Applied += StageApplied;
		Remaining -= StageApplied;

		// Float cancellation can leave a stalled residue; never spin on it.
		if (StageApplied <= UE_KINDA_SMALL_NUMBER && NewP > 0.f)
		{
			break;
		}
	}

	// Refund the un-crossed units, capped by what was actually invested.
	for (const TPair<UItemDefinitionDataAsset*, int32>& Pair : UncrossedUnits)
	{
		int32 Removed = 0;
		for (int32 i = Invested.Num() - 1; i >= 0; --i)
		{
			if (Invested[i].Item != Pair.Key)
			{
				continue;
			}
			Removed = FMath::Min(Pair.Value, Invested[i].Count);
			Invested[i].Count -= Removed;
			if (Invested[i].Count <= 0)
			{
				Invested.RemoveAt(i);
			}
			break;
		}

		const int32 Grant = FMath::FloorToInt32(static_cast<float>(Removed) * RefundFraction);
		if (RefundTarget && Grant > 0)
		{
			const int32 Leftover = RefundTarget->AddItem(Pair.Key, Grant);
			if (Leftover > 0)
			{
				UE_LOG(LogExoneer, Verbose, TEXT("%s: refund of %d x %s did not fit and was lost"),
					*GetNameSafe(Owner), Leftover, *GetNameSafe(Pair.Key));
			}
		}
	}

	if (Applied > 0.f)
	{
		if (StageIndex == 0 && StageProgress01 <= UE_KINDA_SMALL_NUMBER)
		{
			StageProgress01 = 0.f;
			SetPhase(EConstructionPhase::Ghost);
		}
		else if (bWasComplete)
		{
			SetPhase(EConstructionPhase::UnderConstruction);
		}
		OnProgress.Broadcast(GetTotalProgress01());
	}
	return Applied;
}

float UConstructionComponent::GetTotalProgress01() const
{
	if (Phase == EConstructionPhase::Complete)
	{
		return 1.f;
	}

	float TotalWork = 0.f;
	float DoneWork = 0.f;
	for (int32 i = 0; i < Stages.Num(); ++i)
	{
		const float Work = FMath::Max(Stages[i].WeldWork, UE_KINDA_SMALL_NUMBER);
		TotalWork += Work;
		if (i < StageIndex)
		{
			DoneWork += Work;
		}
		else if (i == StageIndex)
		{
			DoneWork += StageProgress01 * Work;
		}
	}
	return TotalWork > 0.f ? FMath::Clamp(DoneWork / TotalWork, 0.f, 1.f) : 0.f;
}

void UConstructionComponent::SetPhase(EConstructionPhase NewPhase)
{
	if (Phase == NewPhase)
	{
		return;
	}
	Phase = NewPhase;
	OnPhaseChanged.Broadcast(Phase);
}

void UConstructionComponent::RestoreState(EConstructionPhase InPhase, int32 InStageIndex, float InStageProgress01)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		UE_LOG(LogExoneer, Warning, TEXT("RestoreState called without authority on %s"), *GetNameSafe(GetOwner()));
		return;
	}

	StageIndex = FMath::Clamp(InStageIndex, 0, FMath::Max(0, Stages.Num() - 1));
	StageProgress01 = FMath::Clamp(InStageProgress01, 0.f, 1.f);
	Phase = InPhase;

	OnPhaseChanged.Broadcast(Phase);
	OnProgress.Broadcast(GetTotalProgress01());
	if (Phase == EConstructionPhase::Complete)
	{
		OnComplete.Broadcast();
	}
}

void UConstructionComponent::RestoreInvested(const TArray<FInventoryEntry>& InInvested)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		UE_LOG(LogExoneer, Warning, TEXT("RestoreInvested called without authority on %s"), *GetNameSafe(GetOwner()));
		return;
	}

	Invested.Reset();
	for (const FInventoryEntry& Entry : InInvested)
	{
		UItemDefinitionDataAsset* Item = Entry.Item.LoadSynchronous();
		if (!Item || Entry.Count <= 0)
		{
			continue;
		}
		FInventoryStack Stack;
		Stack.Item = Item;
		Stack.Count = Entry.Count;
		Invested.Add(Stack);
	}
}

void UConstructionComponent::OnRep_Phase()
{
	OnPhaseChanged.Broadcast(Phase);
	if (Phase == EConstructionPhase::Complete)
	{
		OnComplete.Broadcast();
	}
}

void UConstructionComponent::OnRep_Progress()
{
	OnProgress.Broadcast(GetTotalProgress01());
}
