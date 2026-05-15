// Copyright Exoneer contributors.
#include "Components/OxygenComponent.h"

UOxygenComponent::UOxygenComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

float UOxygenComponent::Deposit(float Amount)
{
	if (Amount <= 0.f) return 0.f;
	const float Space = FMath::Max(0.f, Capacity - Stored);
	const float Taken = FMath::Min(Space, Amount);
	Stored += Taken;
	OnOxygenChanged.Broadcast(Stored, Capacity);
	return Taken;
}

float UOxygenComponent::Withdraw(float Amount)
{
	if (Amount <= 0.f) return 0.f;
	const float Given = FMath::Min(Stored, Amount);
	Stored -= Given;
	OnOxygenChanged.Broadcast(Stored, Capacity);
	return Given;
}
