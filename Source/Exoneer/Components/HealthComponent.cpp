// Copyright Exoneer contributors.
#include "Components/HealthComponent.h"

UHealthComponent::UHealthComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.25f;
}

float UHealthComponent::ApplyDamage(float Amount, EExoneerDamageType Type, AActor* Instigator)
{
	if (bDead || Amount <= 0.f) return 0.f;
	const float Old = Health;
	Health = FMath::Clamp(Health - Amount, 0.f, MaxHealth);
	TimeSinceDamage = 0.f;
	OnHealthChanged.Broadcast(Health, MaxHealth);
	if (Health <= 0.f)
	{
		bDead = true;
		OnDied.Broadcast(Instigator);
	}
	return Old - Health;
}

void UHealthComponent::Heal(float Amount)
{
	if (bDead || Amount <= 0.f) return;
	Health = FMath::Clamp(Health + Amount, 0.f, MaxHealth);
	OnHealthChanged.Broadcast(Health, MaxHealth);
}

void UHealthComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn)
{
	Super::TickComponent(DeltaTime, TickType, TickFn);
	TimeSinceDamage += DeltaTime;
	if (!bDead && TimeSinceDamage >= RegenDelay && Health < MaxHealth)
	{
		Heal(RegenPerSec * DeltaTime);
	}
}
