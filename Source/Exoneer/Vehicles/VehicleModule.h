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

	/**
	 * Actual jet direction in block local axes: the aim axis rotated toward
	 * local +Y by the nozzle cant. The AIM stays LocalThrustAxis, so the build
	 * tool's aim list and the spawner still agree about which orientation
	 * means "thrust up"; only the jet leans, and the block's roll about its
	 * aim axis chooses which way. See
	 * UVehicleBlockDefinitionDataAsset::NozzleCantDeg for why a craft made of
	 * parallel nozzles needs this to have any yaw authority at all.
	 */
	inline FVector LocalThrustDirection(float CantDeg)
	{
		if (FMath::IsNearlyZero(CantDeg))
		{
			return LocalThrustAxis;
		}
		const float Radians = FMath::DegreesToRadians(CantDeg);
		return FVector(-FMath::Cos(Radians), FMath::Sin(Radians), 0.f);
	}
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

	/**
	 * SERVER. Bias away from the pilot's throttle setting, carried between
	 * ticks so the router can rate-limit it at the valve's authored slew rate.
	 *
	 * THREE things move it and nothing else, and all three are torques that do
	 * NOT decay - which is the only kind a rotor cannot afford: cancelling the
	 * craft's STANDING moment, holding a commanded RATE against hull damping
	 * (the sustained part of the attitude command, low-passed), and unwinding
	 * stored rotor momentum. Attitude TRANSIENTS remain the triad's job; they
	 * reach the valve only through that lag, which passes about a seventh of a
	 * quarter-second input. Whatever the bias does is force-nulled before it
	 * is committed, so it moves neither the altitude nor the ground track.
	 */
	float AttitudeTrim = 0.f;
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

/** Solar collector: production scales with the planet's sun fraction and dust opacity. */
UCLASS()
class EXONEER_API USolarModule : public UVehicleModule
{
	GENERATED_BODY()

public:
	virtual float GetCurrentProduction() const override;
};

/** Propellant tank. Fill fraction lives in the record StateScalar. */
UCLASS()
class EXONEER_API UFuelTankModule : public UVehicleModule
{
	GENERATED_BODY()

public:
	virtual void Initialize(AVehicleConstruct* InConstruct, int32 InBlockInstanceId) override;
};

/**
 * Attitude control: a three-axis reaction wheel triad in one block.
 *
 * This class is the ACTUATOR only. The control law lives one level up, in
 * AVehicleConstruct::ServerRouteThrust, because two triads on one hull are one
 * control system with more authority - not two controllers each with its own
 * opinion. The construct hands each block an absolute torque in world axes
 * (already this block's share of the installed rating); the block enforces
 * what its own hardware can do and applies the result.
 *
 * A reaction wheel is an INTERNAL actuator. Spinning its rotors one way
 * torques the hull the other way, so it can point the vehicle but can never
 * change the total angular momentum of hull + rotors, and it saturates once
 * the rotors reach their speed limit. Everything that follows from that is
 * modelled here and nowhere else:
 *
 *   - torque is clamped PER AXIS at the block's rating (a real orthogonal
 *     triad delivers its rating on each axis, so a diagonal command is not
 *     silently cut by 42 percent the way a magnitude clamp cuts it);
 *   - it integrates into a stored momentum vector, capped at the authored
 *     rotor capacity, and a saturated axis stops producing torque in the
 *     winding direction until that momentum is dumped;
 *   - the gyroscopic cross term -w x h is applied, so a wound rotor visibly
 *     resists rotation about the other axes.
 *
 * There is no free momentum sink. Dumping needs an external torque and the
 * construct commands it explicitly: differential thrust in flight, and the
 * ground whenever the craft is resting on it - on its tyres or on its hull,
 * because the reaction comes from the contact and not from the tyre. The
 * rotors never simply forget.
 *
 * The construct also stops the store from filling in the first place, which
 * matters more than the dump: differential thrust cancels a craft's STANDING
 * moment continuously and for free, so the rotors only ever pay for
 * transients. See ExoneerThrust.h.
 */
UCLASS()
class EXONEER_API UGyroModule : public UVehicleModule
{
	GENERATED_BODY()

public:
	virtual void TickModule(float DeltaSeconds) override;
	virtual float GetCurrentDraw() const override;
	virtual void Initialize(AVehicleConstruct* InConstruct, int32 InBlockInstanceId) override;

	/**
	 * SERVER. Absolute torque this block should deliver to the hull, in WORLD
	 * axes and N*m, written by the construct's attitude controller each tick.
	 * Already divided by the number of installed triads.
	 */
	FVector CommandTorqueWorldNm = FVector::ZeroVector;

	/** Rated motor torque per axis (N*m), resolved from the block definition at Initialize. */
	float RatedTorqueNm = 0.f;

	/** Rotor momentum capacity per axis (N*m*s), resolved from the block definition. */
	float MomentumCapacityNms = 0.f;

	/** Fraction of stored momentum shed per second while the craft is supported by the ground (1/s). */
	float GroundBleedPerSec = 0.f;

	/** Stored rotor momentum in CONSTRUCT-LOCAL axes (N*m*s). */
	FVector GetStoredMomentumLocal() const { return StoredMomentumLocal; }

	/** Worst-axis fraction of the momentum envelope in use, 0..1. */
	float GetSaturationFraction() const;

private:
	FVector StoredMomentumLocal = FVector::ZeroVector;

	/** Last commanded torque as a fraction of rating, for the power draw. */
	float LastCommandFraction = 0.f;
};
