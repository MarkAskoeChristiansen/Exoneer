// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "VehicleModule.generated.h"

class AVehicleConstruct;
class UVehicleBlockDefinitionDataAsset;
struct FVehicleBlockRecord;

/**
 * Server-side behavior of one functional vehicle block (thruster, cockpit,
 * battery, solar collector). Instantiated by AVehicleConstruct for every
 * COMPLETE block whose definition names a ModuleClass; destroyed when the
 * block is removed or drops out of Complete.
 *
 * Modules are plain UObjects (outer = the construct) and exist ONLY on the
 * server. Anything clients must see lives in the replicated block record
 * (StateScalar: throttle for thrusters, charge fraction for batteries) or on
 * the construct itself.
 */
UCLASS(Abstract)
class EXONEER_API UVehicleModule : public UObject
{
	GENERATED_BODY()

public:
	/** SERVER. Bind to the construct + record. */
	virtual void Initialize(AVehicleConstruct* InConstruct, int32 InBlockInstanceId);

	/**
	 * SERVER. Called exactly once before the construct drops its reference
	 * (block removed, phase regressed, or records moved by split detection).
	 * Base is empty; overrides must be safe to call mid-tick.
	 */
	virtual void Shutdown() {}

	/** SERVER. Called from the construct's physics tick while functional. */
	virtual void TickModule(float DeltaSeconds) {}

	/** Watts drawn (negative PowerDelta scaled by activity) this tick. */
	virtual float GetCurrentDraw() const;

	/** Watts produced this tick. */
	virtual float GetCurrentProduction() const;

	AVehicleConstruct* GetConstruct() const { return Construct.Get(); }
	int32 GetBlockInstanceId() const { return BlockInstanceId; }

	/** Convenience: fetch my record and definition (may be null mid-removal). */
	const FVehicleBlockRecord* FindRecord() const;
	const UVehicleBlockDefinitionDataAsset* FindDef() const;

protected:
	TWeakObjectPtr<AVehicleConstruct> Construct;
	int32 BlockInstanceId = INDEX_NONE;
};

/** Thruster convention in one place: force acts along block local -X, exhaust out of +X. */
namespace ExoneerThruster
{
	inline const FVector LocalThrustAxis(-1.f, 0.f, 0.f);
}

/**
 * Thrust along the block's local -X face (exhaust out of +X), scaled by
 * throttle and the construct's power supply. Writes throttle to the record's
 * StateScalar for client VFX.
 */
UCLASS()
class EXONEER_API UThrusterModule : public UVehicleModule
{
	GENERATED_BODY()

public:
	virtual void TickModule(float DeltaSeconds) override;
	virtual float GetCurrentDraw() const override;

	/** SERVER. Set by the construct's input router each tick. 0..1. */
	float Throttle = 0.f;
};

/** Pilot seat: routes stored pilot input into construct move/rotate intents. */
UCLASS()
class EXONEER_API UCockpitModule : public UVehicleModule
{
	GENERATED_BODY()

public:
	virtual void TickModule(float DeltaSeconds) override;
};

/** Battery: charges from surplus, discharges to cover deficit (via the construct ledger). */
UCLASS()
class EXONEER_API UBatteryModule : public UVehicleModule
{
	GENERATED_BODY()
};

/** Solar collector: production scales with the planet's sun fraction. */
UCLASS()
class EXONEER_API USolarModule : public UVehicleModule
{
	GENERATED_BODY()

public:
	virtual float GetCurrentProduction() const override;
};

/**
 * Attitude control: a three-axis reaction wheel triad in one block.
 *
 * A reaction wheel is an INTERNAL actuator. Spinning its rotors one way
 * torques the hull the other way, so it can point the vehicle but can never
 * change the total angular momentum of hull + rotors, and it saturates once
 * the rotors reach their speed limit. Both properties are modelled: torque is
 * capped at the block's rating and integrates into a stored momentum vector,
 * and a saturated axis stops producing torque in that direction until the
 * momentum is dumped against an external torque (the ground, through the
 * wheels).
 *
 * The gyroscopic cross term -w x h is applied too: a wound-up rotor visibly
 * resists rotation about the other axes, which is what a gyro stabiliser
 * physically does. At 0.5 rad/s with a full rotor it is larger than the
 * device rating itself, so omitting it would be dropping the dominant term.
 */
UCLASS()
class EXONEER_API UGyroModule : public UVehicleModule
{
	GENERATED_BODY()

public:
	virtual void TickModule(float DeltaSeconds) override;
	virtual float GetCurrentDraw() const override;

	/** SERVER. Desired torque direction in world space, magnitude 0..1. Set by the construct's input router each tick. */
	FVector CommandWorld = FVector::ZeroVector;

	/** Rated torque per axis (N*m), resolved from the block definition at Initialize. */
	float RatedTorqueNm = 0.f;

	virtual void Initialize(AVehicleConstruct* InConstruct, int32 InBlockInstanceId) override;

private:
	/** Stored rotor momentum in CONSTRUCT-LOCAL axes (N*m*s). Saturates at RatedTorqueNm * SaturationSeconds. */
	FVector StoredMomentumLocal = FVector::ZeroVector;

	/** Seconds of full-rating torque the rotors can absorb before saturating. */
	float SaturationSeconds = 5.f;

	/** Last commanded torque magnitude as a fraction of rating, for the power draw. */
	float LastCommandFraction = 0.f;
};
