// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PowerComponent.generated.h"

/**
 * Attached to any base piece that participates in a power network as a
 * consumer (NominalDraw > 0), producer (NominalOutput > 0), or both (battery).
 *
 * The UPowerNetworkComponent on the owning ABaseStructure registers these and
 * simulates the network as a single batch on the server. StoredEnergy and
 * SupplyFraction replicate for HUD/VFX.
 */
UCLASS(ClassGroup = (Exoneer), meta = (BlueprintSpawnableComponent))
class EXONEER_API UPowerComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPowerComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Power") float NominalDraw = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Power") float NominalOutput = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Power") float StorageCapacity = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Power")
	float StoredEnergy = 0.f;

	/** How much of its nominal draw the block actually received last tick. 0..1. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "Power")
	float SupplyFraction = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Replicated, Category = "Power")
	bool bEnabled = true;

	UFUNCTION(BlueprintPure, Category = "Power") bool IsPowered() const { return SupplyFraction >= 0.99f; }
	UFUNCTION(BlueprintPure, Category = "Power") bool IsBattery() const { return StorageCapacity > 0.f; }

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
};
