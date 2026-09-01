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
