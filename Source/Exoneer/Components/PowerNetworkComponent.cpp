// Copyright Exoneer contributors.
#include "Components/PowerNetworkComponent.h"
#include "Components/PowerComponent.h"

UPowerNetworkComponent::UPowerNetworkComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.2f; // 5 Hz event-style simulation
}

void UPowerNetworkComponent::Register(UPowerComponent* Node)
{
	if (Node) Nodes.AddUnique(Node);
}

void UPowerNetworkComponent::Unregister(UPowerComponent* Node)
{
	Nodes.Remove(Node);
}

void UPowerNetworkComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn)
{
	Super::TickComponent(DeltaTime, TickType, TickFn);

	FPowerNetworkSnapshot Snap;
	float TotalProductionW = 0.f;
	float TotalDrawW = 0.f;
	float TotalStored = 0.f;
	float TotalStorageCap = 0.f;

	// Pass 1: producers (incl. batteries' discharge potential), demand, storage capacity.
	for (UPowerComponent* N : Nodes)
	{
		if (!N || !N->bEnabled) continue;
		if (N->NominalOutput > 0.f)
		{
			TotalProductionW += N->NominalOutput;
		}
		if (N->NominalDraw > 0.f)
		{
			TotalDrawW += N->NominalDraw;
		}
		if (N->IsBattery())
		{
			TotalStorageCap += N->StorageCapacity;
			TotalStored += N->StoredEnergy;
		}
	}

	// Compute supply available to consumers this tick.
	float AvailableW = TotalProductionW;
	float NeededDeficitW = FMath::Max(0.f, TotalDrawW - AvailableW);

	// Discharge batteries to make up deficit.
	float DischargeW = 0.f;
	if (NeededDeficitW > 0.f && TotalStored > 0.f)
	{
		const float MaxDischargeJoules = TotalStored;
		const float NeededJoules = NeededDeficitW * DeltaTime;
		const float Joules = FMath::Min(MaxDischargeJoules, NeededJoules);
		DischargeW = Joules / FMath::Max(DeltaTime, KINDA_SMALL_NUMBER);
		AvailableW += DischargeW;

		// Remove energy from batteries proportionally.
		const float Frac = (TotalStored > 0.f) ? (Joules / TotalStored) : 0.f;
		for (UPowerComponent* N : Nodes)
		{
			if (N && N->bEnabled && N->IsBattery())
			{
				N->StoredEnergy = FMath::Max(0.f, N->StoredEnergy * (1.f - Frac));
			}
		}
	}

	const float SupplyFraction = (TotalDrawW > 0.f) ? FMath::Clamp(AvailableW / TotalDrawW, 0.f, 1.f) : 1.f;

	// Pass 2: assign supply fraction to each consumer.
	for (UPowerComponent* N : Nodes)
	{
		if (!N) continue;
		N->SupplyFraction = (N->NominalDraw > 0.f) ? SupplyFraction : 1.f;
	}

	// Surplus production charges batteries.
	float SurplusW = FMath::Max(0.f, TotalProductionW - TotalDrawW);
	if (SurplusW > 0.f)
	{
		float SurplusJoules = SurplusW * DeltaTime;
		// Distribute to batteries that have headroom (equal share).
		TArray<UPowerComponent*> Batteries;
		float TotalHeadroom = 0.f;
		for (UPowerComponent* N : Nodes)
		{
			if (N && N->bEnabled && N->IsBattery())
			{
				const float Headroom = FMath::Max(0.f, N->StorageCapacity - N->StoredEnergy);
				if (Headroom > 0.f)
				{
					Batteries.Add(N);
					TotalHeadroom += Headroom;
				}
			}
		}
		if (TotalHeadroom > 0.f)
		{
			const float ToStore = FMath::Min(SurplusJoules, TotalHeadroom);
			for (UPowerComponent* N : Batteries)
			{
				const float Headroom = FMath::Max(0.f, N->StorageCapacity - N->StoredEnergy);
				const float Share = ToStore * (Headroom / TotalHeadroom);
				N->StoredEnergy = FMath::Min(N->StorageCapacity, N->StoredEnergy + Share);
			}
		}
	}

	// Recompute snapshot stored.
	TotalStored = 0.f;
	for (UPowerComponent* N : Nodes)
	{
		if (N && N->IsBattery()) TotalStored += N->StoredEnergy;
	}

	Snap.TotalProduction = TotalProductionW;
	Snap.TotalDemand = TotalDrawW;
	Snap.TotalStored = TotalStored;
	Snap.TotalStorage = TotalStorageCap;
	Snap.bOverload = (TotalDrawW > TotalProductionW + (DischargeW * 1.05f));
	LastSnapshot = Snap;
	OnPowerNetworkUpdated.Broadcast(Snap);
}
