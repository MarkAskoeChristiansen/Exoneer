// Copyright Exoneer contributors.
#include "Components/PowerNetworkComponent.h"
#include "Components/PowerComponent.h"
#include "GameFramework/Actor.h"
#include "Net/UnrealNetwork.h"

UPowerNetworkComponent::UPowerNetworkComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.2f;   // matches SimInterval default
	SetIsReplicatedByDefault(true);
}

void UPowerNetworkComponent::Register(UPowerComponent* Node)
{
	if (Node)
	{
		Nodes.AddUnique(Node);
	}
}

void UPowerNetworkComponent::Unregister(UPowerComponent* Node)
{
	Nodes.Remove(Node);
}

void UPowerNetworkComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn)
{
	Super::TickComponent(DeltaTime, TickType, TickFn);

	const AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority())
	{
		// Clients consume the replicated snapshot; no local simulation.
		SetComponentTickEnabled(false);
		return;
	}

	// The tick interval IS the sim batch: DeltaTime carries the accumulated
	// wall time since the previous sim. Tracks designer edits to SimInterval.
	PrimaryComponentTick.TickInterval = SimInterval;

	Simulate(DeltaTime);
}

void UPowerNetworkComponent::Simulate(float DeltaSeconds)
{
	const float Dt = FMath::Max(DeltaSeconds, KINDA_SMALL_NUMBER);

	// Drop nodes whose owning piece was destroyed since the last sim.
	Nodes.RemoveAll([](const TObjectPtr<UPowerComponent>& N) { return N == nullptr; });

	// Pass 1: production, demand, and battery totals over enabled nodes.
	float Production = 0.f;
	float Demand = 0.f;
	float Stored = 0.f;
	float Storage = 0.f;
	for (const UPowerComponent* N : Nodes)
	{
		if (!N || !N->bEnabled)
		{
			continue;
		}
		Production += FMath::Max(0.f, N->NominalOutput);
		Demand += FMath::Max(0.f, N->NominalDraw);
		if (N->IsBattery())
		{
			Stored += N->StoredEnergy;
			Storage += N->StorageCapacity;
		}
	}

	// Batteries discharge (proportionally to their charge) to cover a deficit.
	float Available = Production;
	const float DeficitW = FMath::Max(0.f, Demand - Production);
	float DischargeW = 0.f;
	if (DeficitW > 0.f && Stored > 0.f)
	{
		const float Joules = FMath::Min(Stored, DeficitW * Dt);
		DischargeW = Joules / Dt;
		Available += DischargeW;

		const float Frac = Joules / Stored;
		for (UPowerComponent* N : Nodes)
		{
			if (N && N->bEnabled && N->IsBattery())
			{
				N->StoredEnergy = FMath::Max(0.f, N->StoredEnergy * (1.f - Frac));
			}
		}
	}

	// Surplus production charges batteries, split by remaining headroom.
	const float SurplusJoules = FMath::Max(0.f, Production - Demand) * Dt;
	if (SurplusJoules > 0.f)
	{
		float TotalHeadroom = 0.f;
		for (const UPowerComponent* N : Nodes)
		{
			if (N && N->bEnabled && N->IsBattery())
			{
				TotalHeadroom += FMath::Max(0.f, N->StorageCapacity - N->StoredEnergy);
			}
		}
		if (TotalHeadroom > 0.f)
		{
			const float ToStore = FMath::Min(SurplusJoules, TotalHeadroom);
			for (UPowerComponent* N : Nodes)
			{
				if (N && N->bEnabled && N->IsBattery())
				{
					const float Headroom = FMath::Max(0.f, N->StorageCapacity - N->StoredEnergy);
					N->StoredEnergy = FMath::Min(N->StorageCapacity, N->StoredEnergy + ToStore * (Headroom / TotalHeadroom));
				}
			}
		}
	}

	// Pass 2: write supply back to nodes. Disabled nodes receive nothing;
	// enabled zero-draw nodes count as fully supplied.
	const float Fraction = Demand > 0.f ? FMath::Clamp(Available / Demand, 0.f, 1.f) : 1.f;
	float StoredAfter = 0.f;
	for (UPowerComponent* N : Nodes)
	{
		if (!N)
		{
			continue;
		}
		if (!N->bEnabled)
		{
			N->SupplyFraction = 0.f;
			continue;
		}
		N->SupplyFraction = N->NominalDraw > 0.f ? Fraction : 1.f;
		if (N->IsBattery())
		{
			StoredAfter += N->StoredEnergy;
		}
	}

	Snapshot.TotalProduction = Production;
	Snapshot.TotalDemand = Demand;
	Snapshot.TotalStored = StoredAfter;
	Snapshot.TotalStorage = Storage;
	Snapshot.bOverload = Fraction < 1.f - KINDA_SMALL_NUMBER;

	// RepNotify only fires on clients; mirror the broadcast on the server.
	OnPowerNetworkUpdated.Broadcast(Snapshot);
}

void UPowerNetworkComponent::OnRep_Snapshot()
{
	OnPowerNetworkUpdated.Broadcast(Snapshot);
}

void UPowerNetworkComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UPowerNetworkComponent, Snapshot);
}
