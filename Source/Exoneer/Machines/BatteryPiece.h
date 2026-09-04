// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Machines/MachinePiece.h"
#include "BatteryPiece.generated.h"

/**
 * Stores energy for the structure's power network. Storage capacity comes
 * from Def->EnergyStorage; charge/discharge is simulated centrally by the
 * UPowerNetworkComponent on the owning ABaseStructure. Suit recharge is the
 * umbilical port, not a radius around this bank.
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

	virtual void Tick(float DeltaSeconds) override;
};
