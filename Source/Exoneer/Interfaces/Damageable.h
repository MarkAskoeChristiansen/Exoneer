// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "Damageable.generated.h"

UINTERFACE(BlueprintType, MinimalAPI)
class UDamageable : public UInterface { GENERATED_BODY() };

UENUM(BlueprintType)
enum class EExoneerDamageType : uint8
{
	Generic,
	Impact,
	Mining,
	Suffocation,
	Cold,
	Heat,
	Radiation,
	Fall,
	Explosion
};

class EXONEER_API IDamageable
{
	GENERATED_BODY()
public:
	/** Apply damage; returns the actual amount applied after armor/resists. */
	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Damage")
	float ApplyExoneerDamage(float Amount, EExoneerDamageType Type, AActor* Instigator);
	virtual float ApplyExoneerDamage_Implementation(float Amount, EExoneerDamageType Type, AActor* Instigator) { return 0.f; }

	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Damage")
	float GetCurrentHealth() const;
	virtual float GetCurrentHealth_Implementation() const { return 0.f; }

	UFUNCTION(BlueprintNativeEvent, Category = "Exoneer|Damage")
	float GetMaxHealth() const;
	virtual float GetMaxHealth_Implementation() const { return 0.f; }
};
