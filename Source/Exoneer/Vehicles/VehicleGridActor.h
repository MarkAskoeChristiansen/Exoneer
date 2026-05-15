// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Building/BlockGridActor.h"
#include "VehicleGridActor.generated.h"

class ACockpitBlock;
class APawn;

/**
 * A vehicle / ship grid. Extends ABlockGridActor with a simulating physics
 * root, a pilot reference, and mass-based thruster-driven movement.
 *
 * Each tick:
 *  1. The active cockpit forwards player input as MoveInput / RotateInput.
 *  2. The vehicle assigns Throttle to each AThrusterBlock based on whether
 *     that thruster's world-space direction matches the desired translation.
 *  3. All thruster forces are summed and applied to the rigid body at the
 *     centre of mass, then torque is applied for rotation.
 */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API AVehicleGridActor : public ABlockGridActor
{
	GENERATED_BODY()
public:
	AVehicleGridActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly) class UStaticMeshComponent* PhysicsRoot = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vehicle") float HoverHeight = 200.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vehicle") float HoverStrength = 600000.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vehicle") float HoverDamping = 80000.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vehicle") float RotationTorque = 5000000.f;

	UFUNCTION(BlueprintCallable, Category = "Vehicle") void SetActivePilot(APawn* InPilot, ACockpitBlock* Cockpit);
	UFUNCTION(BlueprintCallable, Category = "Vehicle") void ApplyPilotInput(const FVector& MoveInput, const FVector& RotateInput);

	virtual void Tick(float DeltaSeconds) override;

protected:
	UPROPERTY() APawn* ActivePilot = nullptr;
	UPROPERTY() ACockpitBlock* ActiveCockpit = nullptr;

	FVector PendingMove = FVector::ZeroVector;
	FVector PendingRotate = FVector::ZeroVector;

	void ApplyThrust(float DeltaSeconds);
	void ApplyHover(float DeltaSeconds);
};
