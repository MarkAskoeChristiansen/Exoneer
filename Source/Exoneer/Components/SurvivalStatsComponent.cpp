// Copyright Exoneer contributors.
#include "Components/SurvivalStatsComponent.h"
#include "Interfaces/Damageable.h"

USurvivalStatsComponent::USurvivalStatsComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.25f;  // 4 Hz is plenty for survival vitals
}

void USurvivalStatsComponent::AddOxygen(float Amount)
{
	Oxygen = FMath::Clamp(Oxygen + Amount, 0.f, MaxOxygen);
	OnOxygenChanged.Broadcast(GetOxygenNormalized());
}

void USurvivalStatsComponent::AddSuitPower(float Amount)
{
	SuitPower = FMath::Clamp(SuitPower + Amount, 0.f, MaxSuitPower);
	OnSuitPowerChanged.Broadcast(GetSuitPowerNormalized());
}

void USurvivalStatsComponent::AddNutrition(float Amount)
{
	Nutrition = FMath::Clamp(Nutrition + Amount, 0.f, MaxNutrition);
	OnNutritionChanged.Broadcast(GetNutritionNormalized());
}

void USurvivalStatsComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn)
{
	Super::TickComponent(DeltaTime, TickType, TickFn);

	const float DT = DeltaTime;

	// Drain.
	Oxygen = FMath::Clamp(Oxygen - OxygenDrainPerSec * DT, 0.f, MaxOxygen);
	SuitPower = FMath::Clamp(SuitPower - SuitPowerDrainPerSec * DT, 0.f, MaxSuitPower);
	Nutrition = FMath::Clamp(Nutrition - NutritionDrainPerSec * DT, 0.f, MaxNutrition);

	// Temperature drift toward ambient (slower when suit has power).
	const float Insulation = (SuitPower > 0.f) ? 0.25f : 1.f;
	BodyTempC = FMath::FInterpTo(BodyTempC, AmbientTempC, DT, TempEquilibrationRate * Insulation);

	OnOxygenChanged.Broadcast(GetOxygenNormalized());
	OnSuitPowerChanged.Broadcast(GetSuitPowerNormalized());
	OnNutritionChanged.Broadcast(GetNutritionNormalized());
	OnTemperatureChanged.Broadcast(BodyTempC);

	// Damage when critical.
	AActor* Owner = GetOwner();
	if (!Owner) return;
	auto TryDamage = [&](float Amount, EExoneerDamageType Type)
	{
		if (Amount <= 0.f) return;
		if (Owner->Implements<UDamageable>())
		{
			IDamageable::Execute_ApplyExoneerDamage(Owner, Amount, Type, Owner);
		}
	};

	if (Oxygen <= 0.f)    TryDamage(SuffocationDPS * DT, EExoneerDamageType::Suffocation);
	if (Nutrition <= 0.f) TryDamage(StarvationDPS * DT, EExoneerDamageType::Generic);
	if (BodyTempC < MinSafeTempC) TryDamage(TempDamageDPS * DT, EExoneerDamageType::Cold);
	if (BodyTempC > MaxSafeTempC) TryDamage(TempDamageDPS * DT, EExoneerDamageType::Heat);
}
