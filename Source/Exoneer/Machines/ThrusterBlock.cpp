// Copyright Exoneer contributors.
#include "Machines/ThrusterBlock.h"
#include "Components/PowerComponent.h"

AThrusterBlock::AThrusterBlock()
{
}

FVector AThrusterBlock::GetLocalThrustDirection() const
{
	// Rotated around Z by RotationStep * 90 degrees.
	const FRotator R(0.f, 90.f * (RotationStep & 3), 0.f);
	return R.RotateVector(FVector::ForwardVector);
}

FVector AThrusterBlock::GetWorldThrustForce() const
{
	const float Frac = Power ? FMath::Clamp(Power->SupplyFraction, 0.f, 1.f) : 1.f;
	const FVector LocalDir = GetLocalThrustDirection();
	const FVector WorldDir = GetActorTransform().TransformVectorNoScale(LocalDir);
	return WorldDir * MaxThrustN * FMath::Clamp(Throttle, 0.f, 1.f) * Frac;
}
