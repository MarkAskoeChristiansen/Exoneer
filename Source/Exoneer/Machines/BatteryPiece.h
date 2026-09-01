// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Machines/MachinePiece.h"
#include "BatteryPiece.generated.h"

/**
 * Stores energy for the structure's power network. Storage capacity comes
 * from Def->EnergyStorage; charge/discharge is simulated centrally by the
 * UPowerNetworkComponent on the owning ABaseStructure.
 */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API ABatteryPiece : public AMachinePiece
{
	GENERATED_BODY()

public:
	ABatteryPiece();

	/** Fill fraction below which the battery reports LowPower (HUD/VFX). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Machine", meta = (ClampMin = "0", ClampMax = "1"))
	float LowChargeFraction = 0.05f;

	// --- Umbilical prototype (GAME-SCOPE.md module 1): standing near a charged
	// battery bank recharges the engineer's suit from stored energy. ---

	/** Radius (cm) within which suits recharge. 0 disables. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Machine")
	float SuitRechargeRadius = 350.f;

	/** Suit power restored per second per engineer in range. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Machine")
	float SuitRechargePerSec = 6.f;

	/** Watt-seconds of stored energy consumed per suit power unit. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Machine")
	float EnergyPerSuitUnit = 100.f;

	virtual void Tick(float DeltaSeconds) override;
};
