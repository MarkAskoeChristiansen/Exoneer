// Copyright Exoneer contributors.
#include "Machines/BatteryPiece.h"
#include "Components/PowerComponent.h"
#include "Components/ConstructionComponent.h"

ABatteryPiece::ABatteryPiece()
{
	// Server-side state refresh cadence; clients receive MachineState via rep.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.25f;
}

void ABatteryPiece::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority() || !Power)
	{
		return;
	}
	if (Construction && !Construction->IsComplete())
	{
		UpdateMachineState(EMachineState::Idle);
		return;
	}

	// Nearly drained batteries flag LowPower; everything else is Idle
	// (the network component owns the actual charge/discharge simulation).
	const float Fill = Power->StorageCapacity > 0.f ? Power->StoredEnergy / Power->StorageCapacity : 0.f;
	UpdateMachineState(Fill < LowChargeFraction ? EMachineState::LowPower : EMachineState::Idle);
}
