// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Interfaces/Damageable.h"
#include "HealthComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnHealthChanged, float, NewHealth, float, MaxHealth);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDied, AActor*, KilledBy);

UCLASS(ClassGroup = (Exoneer), meta = (BlueprintSpawnableComponent))
class EXONEER_API UHealthComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UHealthComponent();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Health") float MaxHealth = 100.f;
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Health") float Health = 100.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Health") float RegenPerSec = 1.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Health") float RegenDelay = 5.f;

	UPROPERTY(BlueprintAssignable) FOnHealthChanged OnHealthChanged;
	UPROPERTY(BlueprintAssignable) FOnDied OnDied;

	UFUNCTION(BlueprintCallable) float ApplyDamage(float Amount, EExoneerDamageType Type, AActor* Instigator);
	UFUNCTION(BlueprintCallable) void Heal(float Amount);
	UFUNCTION(BlueprintPure) bool IsDead() const { return bDead; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn) override;

protected:
	bool bDead = false;
	float TimeSinceDamage = 9999.f;
};
