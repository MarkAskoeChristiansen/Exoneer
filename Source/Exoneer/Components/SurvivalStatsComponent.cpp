// Copyright Exoneer contributors.
#include "Components/SurvivalStatsComponent.h"
#include "Interfaces/Damageable.h"
#include "Net/UnrealNetwork.h"

USurvivalStatsComponent::USurvivalStatsComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.25f;  // 4 Hz is plenty for survival vitals
	SetIsReplicatedByDefault(true);
}

void USurvivalStatsComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(USurvivalStatsComponent, Oxygen);
	DOREPLIFETIME(USurvivalStatsComponent, SuitPower);
	DOREPLIFETIME(USurvivalStatsComponent, BodyTempC);
	DOREPLIFETIME(USurvivalStatsComponent, AmbientTempC);
}

void USurvivalStatsComponent::AddOxygen(float Amount)
{
	if (!GetOwner() || !GetOwner()->HasAuthority()) return;
	Oxygen = FMath::Clamp(Oxygen + Amount, 0.f, MaxOxygen);
	OnOxygenChanged.Broadcast(GetOxygenNormalized());
}

void USurvivalStatsComponent::AddSuitPower(float Amount)
{
	if (!GetOwner() || !GetOwner()->HasAuthority()) return;
	SuitPower = FMath::Clamp(SuitPower + Amount, 0.f, MaxSuitPower);
	OnSuitPowerChanged.Broadcast(GetSuitPowerNormalized());
}

void USurvivalStatsComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn)
{
	Super::TickComponent(DeltaTime, TickType, TickFn);

	// The SERVER owns the simulation; clients hear about it via RepNotify.
	// Simulating locally as well would silently diverge from the authoritative
	// values the tools drain and gate on.
	AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority())
	{
		return;
	}

	const float DT = DeltaTime;

	// Drain.
	Oxygen = FMath::Clamp(Oxygen - OxygenDrainPerSec * DT, 0.f, MaxOxygen);
	SuitPower = FMath::Clamp(SuitPower - SuitPowerDrainPerSec * DT, 0.f, MaxSuitPower);

	// Temperature drift toward ambient (slower when suit has power).
	const float Insulation = (SuitPower > 0.f) ? 0.25f : 1.f;
	BodyTempC = FMath::FInterpTo(BodyTempC, AmbientTempC, DT, TempEquilibrationRate * Insulation);

	OnOxygenChanged.Broadcast(GetOxygenNormalized());
	OnSuitPowerChanged.Broadcast(GetSuitPowerNormalized());
	OnTemperatureChanged.Broadcast(BodyTempC);

	// Damage when critical.
	auto TryDamage = [&](float Amount, EExoneerDamageType Type)
	{
		if (Amount <= 0.f) return;
		if (Owner->Implements<UDamageable>())
		{
			IDamageable::Execute_ApplyExoneerDamage(Owner, Amount, Type, Owner);
		}
	};

	if (Oxygen <= 0.f)    TryDamage(SuffocationDPS * DT, EExoneerDamageType::Suffocation);
	if (BodyTempC < MinSafeTempC) TryDamage(TempDamageDPS * DT, EExoneerDamageType::Cold);
	if (BodyTempC > MaxSafeTempC) TryDamage(TempDamageDPS * DT, EExoneerDamageType::Heat);
}
