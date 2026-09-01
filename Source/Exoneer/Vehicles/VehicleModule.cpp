// Copyright Exoneer contributors.
#include "Vehicles/VehicleModule.h"
#include "Vehicles/VehicleConstruct.h"
#include "Data/VehicleBlockDefinitionDataAsset.h"
#include "Interfaces/Damageable.h"
#include "Interfaces/Pilotable.h"
#include "Vehicles/ExoneerVehicleUnits.h"
#include "Vehicles/VehicleWheelState.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Pawn.h"

void UVehicleModule::Initialize(AVehicleConstruct* InConstruct, int32 InBlockInstanceId)
{
	Construct = InConstruct;
	BlockInstanceId = InBlockInstanceId;
}

const FVehicleBlockRecord* UVehicleModule::FindRecord() const
{
	const AVehicleConstruct* Owner = Construct.Get();
	return Owner ? Owner->FindRecord(BlockInstanceId) : nullptr;
}

const UVehicleBlockDefinitionDataAsset* UVehicleModule::FindDef() const
{
	const FVehicleBlockRecord* Record = FindRecord();
	return Record ? Record->Def.Get() : nullptr;
}

float UVehicleModule::GetCurrentDraw() const
{
	// Default: a passive consumer draws its full negative PowerDelta.
	const UVehicleBlockDefinitionDataAsset* Def = FindDef();
	return (Def && Def->PowerDelta < 0.f) ? -Def->PowerDelta : 0.f;
}

float UVehicleModule::GetCurrentProduction() const
{
	// Default: a passive producer supplies its full positive PowerDelta.
	const UVehicleBlockDefinitionDataAsset* Def = FindDef();
	return (Def && Def->PowerDelta > 0.f) ? Def->PowerDelta : 0.f;
}

// --- Thruster ---

void UThrusterModule::TickModule(float DeltaSeconds)
{
	AVehicleConstruct* Owner = GetConstruct();
	const FVehicleBlockRecord* Record = FindRecord();
	const UVehicleBlockDefinitionDataAsset* Def = FindDef();
	if (!Owner || !Record || !Def || !Owner->PhysicsRoot || Throttle <= KINDA_SMALL_NUMBER)
	{
		return;
	}
	if (!Owner->PhysicsRoot->IsSimulatingPhysics())
	{
		return;
	}

	// Thrust along the block's local -X, scaled by throttle and the power
	// ledger. MaxThrust is authored in Newtons; the shared conversion lives in
	// ExoneerVehicleUnits.h (1 N = 100 kg*cm/s^2).
	const FTransform BlockTransform = Owner->GetBlockWorldTransform(*Record);
	const FVector ThrustDirection = -BlockTransform.GetUnitAxis(EAxis::X);
	const float ForceNewtons = Def->MaxThrust * Throttle * Owner->PowerSupplyFraction;
	Owner->PhysicsRoot->AddForceAtLocation(ThrustDirection * ForceNewtons * ExoneerUnits::NewtonsToUEForce, BlockTransform.GetLocation());
}

float UThrusterModule::GetCurrentDraw() const
{
	// Thrusters author PowerDelta negative; draw scales with throttle.
	const UVehicleBlockDefinitionDataAsset* Def = FindDef();
	return Def ? FMath::Max(0.f, -Def->PowerDelta * Throttle) : 0.f;
}

// --- Cockpit ---

void UCockpitModule::TickModule(float DeltaSeconds)
{
	// Validate the seated pilot every tick; a dead, destroyed, or detached
	// pawn must not keep the construct in a piloted state.
	AVehicleConstruct* Owner = GetConstruct();
	if (!Owner || Owner->ActiveCockpitId != GetBlockInstanceId())
	{
		return;
	}

	APawn* Pilot = Owner->PilotPawn;
	if (!IsValid(Pilot))
	{
		// The pawn is gone entirely; ExitPilot cannot detach it, so clear directly.
		Owner->PilotPawn = nullptr;
		Owner->ActiveCockpitId = INDEX_NONE;
		return;
	}

	const bool bDetached = Pilot->GetAttachParentActor() != Owner;
	bool bDead = false;
	if (Pilot->GetClass()->ImplementsInterface(UDamageable::StaticClass()))
	{
		bDead = IDamageable::Execute_GetCurrentHealth(Pilot) <= 0.f;
	}
	if (bDetached || bDead)
	{
		IPilotable::Execute_ExitPilot(Owner, Pilot);
	}
}

// --- Attitude gyro (reaction wheel triad) ---

void UGyroModule::Initialize(AVehicleConstruct* InConstruct, int32 InBlockInstanceId)
{
	Super::Initialize(InConstruct, InBlockInstanceId);
	if (const UVehicleBlockDefinitionDataAsset* Def = FindDef())
	{
		RatedTorqueNm = FMath::Max(0.f, Def->MaxGyroTorqueNm);
	}
}

void UGyroModule::TickModule(float DeltaSeconds)
{
	AVehicleConstruct* Owner = GetConstruct();
	if (!Owner || !Owner->PhysicsRoot || !Owner->PhysicsRoot->IsSimulatingPhysics()
		|| RatedTorqueNm <= 0.f || DeltaSeconds <= 0.f)
	{
		LastCommandFraction = 0.f;
		return;
	}

	const FQuat BodyQuat = Owner->GetActorQuat();
	const FVector AngularVelocity = Owner->PhysicsRoot->GetPhysicsAngularVelocityInRadians();
	const float MomentumLimit = RatedTorqueNm * SaturationSeconds;

	// Commanded torque, capped at the rating.
	FVector CommandTorqueWorld = CommandWorld.GetClampedToMaxSize(1.f) * RatedTorqueNm;

	// Saturation: a rotor already at its speed limit cannot absorb more
	// momentum in that direction, so cancel the component of the command
	// that would wind it further. The opposite direction still works, which
	// is what lets a saturated gyro recover as soon as you steer back.
	const FVector MomentumWorld = BodyQuat.RotateVector(StoredMomentumLocal);
	if (!MomentumWorld.IsNearlyZero() && MomentumWorld.Size() >= MomentumLimit)
	{
		const FVector SaturatedDir = MomentumWorld.GetSafeNormal();
		// The rotor winds up opposite the torque it delivers to the hull.
		const float WindingComponent = -FVector::DotProduct(CommandTorqueWorld, SaturatedDir);
		if (WindingComponent > 0.f)
		{
			CommandTorqueWorld += SaturatedDir * WindingComponent;
		}
	}

	LastCommandFraction = FMath::Clamp(CommandTorqueWorld.Size() / RatedTorqueNm, 0.f, 1.f);

	// Brownouts weaken attitude control physically - the motors are simply
	// given less current.
	CommandTorqueWorld *= FMath::Clamp(Owner->PowerSupplyFraction, 0.f, 1.f);

	// Gyroscopic reaction: -w x h. A wound rotor resists rotation about the
	// other axes; this is the term that makes a spun-up gyro feel like one.
	const FVector CrossTerm = FVector::CrossProduct(AngularVelocity, MomentumWorld);
	const FVector TorqueOnHull = CommandTorqueWorld - CrossTerm;

	if (!TorqueOnHull.IsNearlyZero())
	{
		Owner->PhysicsRoot->AddTorqueInRadians(TorqueOnHull * ExoneerUnits::NmToUETorque);
	}

	// Integrate rotor momentum: what the hull gains, the rotors lose.
	StoredMomentumLocal -= BodyQuat.UnrotateVector(CommandTorqueWorld) * DeltaSeconds;

	// Momentum dumping. A reaction wheel can only shed stored momentum
	// against an EXTERNAL torque; on the ground the wheels supply exactly
	// that through ground friction, so the rotors bleed down while any wheel
	// is in contact. Airborne, the momentum stays - saturate in flight and
	// you must land to reset, which is the honest constraint.
	const FVehicleDrivetrainSummary Drivetrain = Owner->GetDrivetrainSummary();
	if (Drivetrain.WheelsInContact > 0)
	{
		const float BleedPerSecond = 0.25f;   // ~4 s time constant
		StoredMomentumLocal *= FMath::Max(0.f, 1.f - BleedPerSecond * DeltaSeconds);
	}
	StoredMomentumLocal = StoredMomentumLocal.GetClampedToMaxSize(MomentumLimit);
}

float UGyroModule::GetCurrentDraw() const
{
	// Draw follows commanded torque (motor current is proportional to
	// torque), plus a small always-on electronics/bearing load. A parked gyro
	// must not burn its full rating forever.
	const UVehicleBlockDefinitionDataAsset* Def = FindDef();
	const float Rated = Def ? FMath::Max(0.f, -Def->PowerDelta) : 0.f;
	return Rated * (0.05f + 0.95f * LastCommandFraction);
}

// --- Solar collector ---

float USolarModule::GetCurrentProduction() const
{
	const AVehicleConstruct* Owner = GetConstruct();
	const UVehicleBlockDefinitionDataAsset* Def = FindDef();
	if (!Owner || !Def)
	{
		return 0.f;
	}
	return FMath::Max(0.f, Def->PowerDelta) * Owner->GetSunFraction();
}
