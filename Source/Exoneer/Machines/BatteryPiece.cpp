// Copyright Exoneer contributors.
#include "Machines/BatteryPiece.h"
#include "Components/PowerComponent.h"
#include "Components/ConstructionComponent.h"
#include "Components/SurvivalStatsComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"

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

	// Umbilical prototype: engineers near a charged bank sip stored energy
	// into their suits. The tick runs at 4 Hz, so scale by the real interval.
	if (SuitRechargeRadius > 0.f && Power->StoredEnergy > 0.f && EnergyPerSuitUnit > 0.f)
	{
		const float RadiusSq = FMath::Square(SuitRechargeRadius);
		for (TActorIterator<APawn> It(GetWorld()); It; ++It)
		{
			APawn* Pawn = *It;
			if (!IsValid(Pawn) || FVector::DistSquared(Pawn->GetActorLocation(), GetActorLocation()) > RadiusSq)
			{
				continue;
			}
			USurvivalStatsComponent* Stats = Pawn->FindComponentByClass<USurvivalStatsComponent>();
			if (!Stats || Stats->SuitPower >= Stats->MaxSuitPower)
			{
				continue;
			}
			const float Wanted = FMath::Min(SuitRechargePerSec * DeltaSeconds, Stats->MaxSuitPower - Stats->SuitPower);
			const float Affordable = FMath::Min(Wanted, Power->StoredEnergy / EnergyPerSuitUnit);
			if (Affordable > 0.f)
			{
				Stats->AddSuitPower(Affordable);
				Power->StoredEnergy = FMath::Max(0.f, Power->StoredEnergy - Affordable * EnergyPerSuitUnit);
			}
		}
	}

	// Nearly drained batteries flag LowPower; everything else is Idle
	// (the network component owns the actual charge/discharge simulation).
	const float Fill = Power->StorageCapacity > 0.f ? Power->StoredEnergy / Power->StorageCapacity : 0.f;
	UpdateMachineState(Fill < LowChargeFraction ? EMachineState::LowPower : EMachineState::Idle);
}
