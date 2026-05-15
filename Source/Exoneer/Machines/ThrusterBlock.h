// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Machines/MachineBlock.h"
#include "ThrusterBlock.generated.h"

UENUM(BlueprintType)
enum class EThrusterClass : uint8
{
	Small,
	Large,
	Atmospheric
};

/**
 * A directional thrust block. Its forward direction (relative to the grid)
 * is +X rotated by the block's RotationStep. The vehicle grid sums each
 * thruster's thrust scaled by Throttle [0..1] every physics tick.
 */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API AThrusterBlock : public AMachineBlock
{
	GENERATED_BODY()
public:
	AThrusterBlock();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thruster") EThrusterClass Class = EThrusterClass::Small;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thruster") float MaxThrustN = 250000.f;

	/** 0..1 throttle assigned by the vehicle. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Thruster") float Throttle = 0.f;

	/** Forward thrust direction in the grid's local frame. */
	UFUNCTION(BlueprintPure, Category = "Thruster") FVector GetLocalThrustDirection() const;

	/** Current thrust force vector in world space. */
	UFUNCTION(BlueprintPure, Category = "Thruster") FVector GetWorldThrustForce() const;
};
