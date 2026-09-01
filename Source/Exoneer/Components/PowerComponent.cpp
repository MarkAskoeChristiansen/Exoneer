// Copyright Exoneer contributors.
#include "Components/PowerComponent.h"
#include "Net/UnrealNetwork.h"

UPowerComponent::UPowerComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UPowerComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UPowerComponent, StoredEnergy);
	DOREPLIFETIME(UPowerComponent, SupplyFraction);
	DOREPLIFETIME(UPowerComponent, bEnabled);
}
