// Copyright Exoneer contributors.
#include "Machines/UmbilicalPortPiece.h"
#include "Machines/OxygenGeneratorPiece.h"
#include "Building/BaseStructure.h"
#include "Components/ConstructionComponent.h"
#include "Components/InventoryComponent.h"
#include "Components/OxygenComponent.h"
#include "Components/PowerComponent.h"
#include "Components/SurvivalStatsComponent.h"
#include "Data/ItemDefinitionDataAsset.h"
#include "ExoneerGameplayTags.h"
#include "Maintenance/ExoneerMaintenance.h"
#include "Player/PlayerSurvivalCharacter.h"
#include "Engine/AssetManager.h"

namespace
{
	UItemDefinitionDataAsset* FindItemById(FName ItemId)
	{
		if (ItemId.IsNone())
		{
			return nullptr;
		}
		const FPrimaryAssetId AssetId(TEXT("Item"), ItemId);
		if (UItemDefinitionDataAsset* Item = Cast<UItemDefinitionDataAsset>(UAssetManager::Get().GetPrimaryAssetObject(AssetId)))
		{
			return Item;
		}
		const FSoftObjectPath Path = UAssetManager::Get().GetPrimaryAssetPath(AssetId);
		return Cast<UItemDefinitionDataAsset>(Path.TryLoad());
	}

	bool InventoryHas(const UInventoryComponent* Inventory, FName ItemId)
	{
		UItemDefinitionDataAsset* Item = FindItemById(ItemId);
		return Item && Inventory && Inventory->GetItemCount(Item) > 0;
	}
}

AUmbilicalPortPiece::AUmbilicalPortPiece()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.25f;
	PortPowerW = ExoneerMaintenance::UmbilicalPortPowerW;
	PortO2Lps = ExoneerMaintenance::UmbilicalO2MakeupLps;
}

void AUmbilicalPortPiece::ApplyDefinitionStats()
{
	Super::ApplyDefinitionStats();
	if (Power)
	{
		Power->NominalDraw = PortPowerW;
		Power->NominalOutput = 0.f;
	}
}

void AUmbilicalPortPiece::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (HasAuthority())
	{
		DisconnectConnected(/*bRangeBreak*/ false);
	}
	Super::EndPlay(EndPlayReason);
}

void AUmbilicalPortPiece::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority())
	{
		return;
	}

	APlayerSurvivalCharacter* Pawn = ConnectedPawn.Get();
	if (!Pawn)
	{
		if (Power)
		{
			Power->NominalDraw = 0.f;
		}
		return;
	}

	if (!IsValid(Pawn) || (Construction && !Construction->IsComplete()))
	{
		DisconnectConnected(/*bRangeBreak*/ false);
		return;
	}

	const float DistSq = FVector::DistSquared(Pawn->GetActorLocation(), GetActorLocation());
	if (DistSq > FMath::Square(UmbilicalLengthCm))
	{
		DisconnectConnected(/*bRangeBreak*/ true);
		return;
	}

	TransferToConnected(DeltaSeconds);
}

void AUmbilicalPortPiece::TransferToConnected(float DeltaSeconds)
{
	APlayerSurvivalCharacter* Pawn = ConnectedPawn.Get();
	USurvivalStatsComponent* Stats = Pawn ? Pawn->Survival : nullptr;
	if (!Stats)
	{
		return;
	}

	const bool bNeedsPower = Stats->SuitPower < Stats->SuitPowerCapacityKJ - KINDA_SMALL_NUMBER;
	if (Power)
	{
		Power->NominalDraw = bNeedsPower ? PortPowerW : 0.f;
		if (bNeedsPower && Power->bEnabled)
		{
			const float ChargeKJ = PortPowerW * FMath::Clamp(Power->SupplyFraction, 0.f, 1.f) * DeltaSeconds / 1000.f;
			if (ChargeKJ > 0.f)
			{
				Stats->AddSuitPower(ChargeKJ);
			}
		}
	}

	float O2Wanted = FMath::Min(PortO2Lps * DeltaSeconds, Stats->SuitO2CapacityL - Stats->Oxygen);
	if (O2Wanted > 0.f && OwningStructure)
	{
		for (ABasePiece* Piece : OwningStructure->Pieces)
		{
			AOxygenGeneratorPiece* Gen = Cast<AOxygenGeneratorPiece>(Piece);
			if (!Gen || !Gen->Oxygen || (Gen->Construction && !Gen->Construction->IsComplete()))
			{
				continue;
			}
			const float Given = Gen->Oxygen->Withdraw(O2Wanted);
			if (Given > 0.f)
			{
				Stats->AddOxygen(Given);
				O2Wanted -= Given;
			}
			if (O2Wanted <= KINDA_SMALL_NUMBER)
			{
				break;
			}
		}
	}
}

void AUmbilicalPortPiece::DisconnectConnected(bool bRangeBreak)
{
	if (APlayerSurvivalCharacter* Pawn = ConnectedPawn.Get())
	{
		ConnectedPawn.Reset();
		Pawn->SetUmbilicalSource(nullptr, bRangeBreak);
	}
	ConnectedPawn.Reset();
	if (Power)
	{
		Power->NominalDraw = 0.f;
	}
}

void AUmbilicalPortPiece::NotifyPawnDisconnected(APlayerSurvivalCharacter* Pawn)
{
	if (ConnectedPawn.Get() == Pawn)
	{
		ConnectedPawn.Reset();
		if (Power)
		{
			Power->NominalDraw = 0.f;
		}
	}
}

EUmbilicalVerb AUmbilicalPortPiece::ResolveVerb(const APlayerSurvivalCharacter* User) const
{
	if (!User || (Construction && !Construction->IsComplete()))
	{
		return EUmbilicalVerb::None;
	}

	const bool bConnectedHere = User->UmbilicalSource == this;
	if (!bConnectedHere)
	{
		return EUmbilicalVerb::Connect;
	}

	const USurvivalStatsComponent* Stats = User->Survival;
	const uint8 PatchCount = Stats ? Stats->SuitCondition.PatchCount : 0;
	const float Leak = Stats ? Stats->SuitCondition.LeakRateLps : 0.f;
	const UInventoryComponent* Inv = User->Inventory;

	if (PatchCount >= ExoneerMaintenance::SealPatchCap && InventoryHas(Inv, TEXT("suit_seal")))
	{
		return EUmbilicalVerb::ReplaceSeal;
	}
	if (Leak > ExoneerMaintenance::SealEmergencyLps
		&& PatchCount < ExoneerMaintenance::SealPatchCap
		&& InventoryHas(Inv, TEXT("seal_kit")))
	{
		return EUmbilicalVerb::PatchSeal;
	}
	return EUmbilicalVerb::Disconnect;
}

FText AUmbilicalPortPiece::GetPromptFor(const APlayerSurvivalCharacter* User) const
{
	switch (ResolveVerb(User))
	{
	case EUmbilicalVerb::PatchSeal:    return NSLOCTEXT("Exoneer", "UmbilicalPatch", "Patch seal");
	case EUmbilicalVerb::ReplaceSeal:  return NSLOCTEXT("Exoneer", "UmbilicalReplace", "Seal worn out: replace");
	case EUmbilicalVerb::Disconnect:   return NSLOCTEXT("Exoneer", "UmbilicalDisconnect", "Disconnect umbilical");
	case EUmbilicalVerb::Connect:      return NSLOCTEXT("Exoneer", "UmbilicalConnect", "Connect umbilical");
	default:                           return FText::GetEmpty();
	}
}

FText AUmbilicalPortPiece::GetInteractionPrompt_Implementation() const
{
	return NSLOCTEXT("Exoneer", "UmbilicalConnect", "Connect umbilical");
}

FGameplayTagContainer AUmbilicalPortPiece::GetInteractionTags_Implementation() const
{
	FGameplayTagContainer InteractionTags;
	if (Construction && Construction->IsComplete())
	{
		InteractionTags.AddTag(ExoneerTags::Interaction_Connect);
	}
	return InteractionTags;
}

void AUmbilicalPortPiece::OnInteractLocal_Implementation(AActor* Interactor)
{
	// No machine UI. The cable and vitals line are the feedback.
}

bool AUmbilicalPortPiece::OnInteract_Implementation(AActor* Interactor)
{
	if (!HasAuthority() || (Construction && !Construction->IsComplete()))
	{
		return false;
	}

	APlayerSurvivalCharacter* User = Cast<APlayerSurvivalCharacter>(Interactor);
	if (!User)
	{
		return false;
	}

	switch (ResolveVerb(User))
	{
	case EUmbilicalVerb::PatchSeal:
		return User->Survival && User->Survival->TryPatchSeal(User->Inventory);

	case EUmbilicalVerb::ReplaceSeal:
		return User->Survival && User->Survival->TryReplaceSeal(User->Inventory);

	case EUmbilicalVerb::Disconnect:
		if (User->UmbilicalSource == this)
		{
			DisconnectConnected(/*bRangeBreak*/ false);
			return true;
		}
		return false;

	case EUmbilicalVerb::Connect:
		if (APlayerSurvivalCharacter* Previous = ConnectedPawn.Get())
		{
			if (Previous != User)
			{
				DisconnectConnected(/*bRangeBreak*/ false);
			}
		}
		ConnectedPawn = User;
		User->SetUmbilicalSource(this, /*bRangeBreak*/ false);
		return true;

	default:
		return false;
	}
}
