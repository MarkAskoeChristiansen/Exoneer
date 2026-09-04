// Copyright Exoneer contributors.
#include "Components/SurvivalStatsComponent.h"
#include "Components/InventoryComponent.h"
#include "Data/ItemDefinitionDataAsset.h"
#include "Interfaces/Damageable.h"
#include "Maintenance/ExoneerMaintenance.h"
#include "Engine/AssetManager.h"
#include "Net/UnrealNetwork.h"

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

	bool ConsumeOne(UInventoryComponent* Source, FName ItemId)
	{
		UItemDefinitionDataAsset* Item = FindItemById(ItemId);
		if (!Item || !Source || Source->GetItemCount(Item) < 1)
		{
			return false;
		}
		return Source->RemoveItem(Item, 1) >= 1;
	}
}

USurvivalStatsComponent::USurvivalStatsComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.25f;  // 4 Hz is plenty for survival vitals
	SetIsReplicatedByDefault(true);
}

void USurvivalStatsComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(USurvivalStatsComponent, Oxygen);
	DOREPLIFETIME(USurvivalStatsComponent, SuitPower);
	DOREPLIFETIME(USurvivalStatsComponent, BodyTempC);
	DOREPLIFETIME(USurvivalStatsComponent, AmbientTempC);
	DOREPLIFETIME(USurvivalStatsComponent, SuitCondition);
}

void USurvivalStatsComponent::AddOxygen(float Amount)
{
	if (!GetOwner() || !GetOwner()->HasAuthority()) return;
	Oxygen = FMath::Clamp(Oxygen + Amount, 0.f, SuitO2CapacityL);
	OnOxygenChanged.Broadcast(GetOxygenNormalized());
}

void USurvivalStatsComponent::AddSuitPower(float Amount)
{
	if (!GetOwner() || !GetOwner()->HasAuthority()) return;
	SuitPower = FMath::Clamp(SuitPower + Amount, 0.f, SuitPowerCapacityKJ);
	OnSuitPowerChanged.Broadcast(GetSuitPowerNormalized());
}

void USurvivalStatsComponent::AddLeakRateLps(float DeltaLps)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || DeltaLps <= 0.f)
	{
		return;
	}
	SuitCondition.LeakRateLps = FMath::Max(0.f, SuitCondition.LeakRateLps + DeltaLps);
}

bool USurvivalStatsComponent::TryPatchSeal(UInventoryComponent* Source)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return false;
	}
	if (SuitCondition.PatchCount >= ExoneerMaintenance::SealPatchCap)
	{
		return false;
	}
	if (SuitCondition.LeakRateLps <= ExoneerMaintenance::SealEmergencyLps)
	{
		return false;
	}
	if (!ConsumeOne(Source, TEXT("seal_kit")))
	{
		return false;
	}
	SuitCondition.LeakRateLps = ExoneerMaintenance::SealLeakAfterPatch(SuitCondition.PatchCount);
	SuitCondition.PatchCount = uint8(SuitCondition.PatchCount + 1);
	return true;
}

bool USurvivalStatsComponent::TryReplaceSeal(UInventoryComponent* Source)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return false;
	}
	if (SuitCondition.PatchCount < ExoneerMaintenance::SealPatchCap)
	{
		return false;
	}
	if (!ConsumeOne(Source, TEXT("suit_seal")))
	{
		return false;
	}
	SuitCondition.LeakRateLps = 0.f;
	SuitCondition.PatchCount = 0;
	return true;
}

float USurvivalStatsComponent::GetOxygenDrainLps() const
{
	return FMath::Max(MetabolicO2Lps, 0.f) + FMath::Max(SuitCondition.LeakRateLps, 0.f);
}

void USurvivalStatsComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn)
{
	Super::TickComponent(DeltaTime, TickType, TickFn);

	// The SERVER owns the simulation; clients hear about it via RepNotify.
	// Simulating locally as well would silently diverge from the authoritative
	// values the tools drain and gate on.
	AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority())
	{
		return;
	}

	const float DT = DeltaTime;

	Oxygen = FMath::Clamp(Oxygen - GetOxygenDrainLps() * DT, 0.f, SuitO2CapacityL);
	SuitPower = FMath::Clamp(SuitPower - GetSuitPowerDrainKJps() * DT, 0.f, SuitPowerCapacityKJ);

	// Temperature drift toward ambient (slower when suit has power).
	const float Insulation = (SuitPower > 0.f) ? 0.25f : 1.f;
	BodyTempC = FMath::FInterpTo(BodyTempC, AmbientTempC, DT, TempEquilibrationRate * Insulation);

	OnOxygenChanged.Broadcast(GetOxygenNormalized());
	OnSuitPowerChanged.Broadcast(GetSuitPowerNormalized());
	OnTemperatureChanged.Broadcast(BodyTempC);

	auto TryDamage = [&](float Amount, EExoneerDamageType Type)
	{
		if (Amount <= 0.f) return;
		if (Owner->Implements<UDamageable>())
		{
			IDamageable::Execute_ApplyExoneerDamage(Owner, Amount, Type, Owner);
		}
	};

	if (Oxygen <= 0.f)    TryDamage(SuffocationDPS * DT, EExoneerDamageType::Suffocation);
	if (BodyTempC < MinSafeTempC) TryDamage(TempDamageDPS * DT, EExoneerDamageType::Cold);
	if (BodyTempC > MaxSafeTempC) TryDamage(TempDamageDPS * DT, EExoneerDamageType::Heat);
}
