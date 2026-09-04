// Copyright Exoneer contributors.
#include "Vehicles/VehicleModule.h"
#include "Vehicles/VehicleConstruct.h"
#include "Data/VehicleBlockDefinitionDataAsset.h"
#include "Interfaces/Damageable.h"
#include "Interfaces/Pilotable.h"
#include "Vehicles/ExoneerVehicleUnits.h"
#include "Vehicles/ExoneerAttitude.h"
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
	const FVector ThrustDirection = BlockTransform.TransformVectorNoScale(
		ExoneerThruster::LocalThrustDirection(Def->NozzleCantDeg)).GetSafeNormal();
	float ForceNewtons = Def->MaxThrust * Throttle * Owner->PowerSupplyFraction;
	// Fuel is optional until a tank is installed. Then mass flow is required.
	if (Owner->HasFuelCapacity())
	{
		const float BurnKg = 0.002f * ForceNewtons * DeltaSeconds;
		if (BurnKg > 0.f && !Owner->ConsumeFuelKg(BurnKg))
		{
			return;
		}
	}
	if (ForceNewtons <= KINDA_SMALL_NUMBER)
	{
		return;
	}
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
//
// Actuator only. The law is in AVehicleConstruct::ServerRouteThrust; see
// ExoneerAttitude.h for the physics this class enforces.

void UGyroModule::Initialize(AVehicleConstruct* InConstruct, int32 InBlockInstanceId)
{
	Super::Initialize(InConstruct, InBlockInstanceId);
	if (const UVehicleBlockDefinitionDataAsset* Def = FindDef())
	{
		RatedTorqueNm = FMath::Max(0.f, Def->MaxGyroTorqueNm);
		MomentumCapacityNms = FMath::Max(0.f, Def->GyroMomentumCapacityNms);
		GroundBleedPerSec = FMath::Max(0.f, Def->MomentumGroundBleedPerSec);
	}
}

float UGyroModule::GetSaturationFraction() const
{
	return ExoneerAttitude::SaturationFraction(StoredMomentumLocal, MomentumCapacityNms);
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

	// The triad's axes are the hull's axes (the block is mounted square), so
	// the rating and the rotor capacity are both per-axis limits in this frame.
	FVector CommandLocal = BodyQuat.UnrotateVector(CommandTorqueWorldNm).BoundToCube(RatedTorqueNm);
	CommandLocal = ExoneerAttitude::ApplySaturation(CommandLocal, StoredMomentumLocal, MomentumCapacityNms);

	LastCommandFraction = FMath::Clamp(static_cast<float>(CommandLocal.GetAbsMax()) / RatedTorqueNm, 0.f, 1.f);

	// Brownouts weaken attitude control physically - the motors are simply
	// given less current.
	CommandLocal *= FMath::Clamp(Owner->PowerSupplyFraction, 0.f, 1.f);

	const FVector MomentumWorld = BodyQuat.RotateVector(StoredMomentumLocal);
	const FVector CommandWorld = BodyQuat.RotateVector(CommandLocal);

	// THE CROSS TERM IS EVALUATED AT THE END-OF-STEP RATE, not the start.
	// -w x h is perpendicular to w analytically, so it transports no power -
	// but that is only true INSTANTANEOUSLY, and explicit Euler on a
	// perpendicular torque adds rotational energy at about
	// 0.5*dt*|tau|^2/I every step. With a pilot aboard the rate null swamps
	// it; with the seat empty the command is exactly zero, the cross term is
	// the only torque on the hull, and there is no airborne momentum sink - so
	// an abandoned craft with wound rotors used to precess FASTER and faster
	// all the way to the ground. That is the owner's "leaving the vehicle mid
	// air" in a slower form. One Picard step toward the implicit solution
	// makes the same term dissipative instead of generative, which is the safe
	// side of neutral and costs one cross product.
	FVector RateForCross = AngularVelocity;
	const FVector Inertia = Owner->GetBodyInertiaKgM2();
	if (DeltaSeconds > 0.f && Inertia.GetMin() > KINDA_SMALL_NUMBER)
	{
		const FVector FirstGuess = CommandWorld + ExoneerAttitude::GyroscopicTorque(RateForCross, MomentumWorld);
		const FVector AlphaLocal = BodyQuat.UnrotateVector(FirstGuess) / Inertia;
		RateForCross += BodyQuat.RotateVector(AlphaLocal) * DeltaSeconds;
	}
	const FVector TorqueOnHull = CommandWorld
		+ ExoneerAttitude::GyroscopicTorque(RateForCross, MomentumWorld);

	if (!TorqueOnHull.IsNearlyZero())
	{
		Owner->PhysicsRoot->AddTorqueInRadians(TorqueOnHull * ExoneerUnits::NmToUETorque);
	}

	// Integrate rotor momentum: what the hull gains, the rotors lose.
	StoredMomentumLocal -= CommandLocal * DeltaSeconds;

	// Momentum leaves the rotors only through an EXTERNAL torque, and a craft
	// resting on the ground has one. The gate is external SUPPORT, not tyre
	// compression: the reaction torque comes from the contact, so a hull lying
	// on the ground supplies exactly what a loaded tyre supplies. Reading the
	// wheel predicate instead meant a craft built with no wheel blocks had an
	// empty wheel array, a permanently false gate, and therefore no momentum
	// sink at all - one saturated axis and that vehicle never flew again,
	// while the comment three lines up claimed the opposite.
	//
	// The rate is authored on the block rather than derived: it stands in for
	// the reaction torque instead of modelling it, and that derivation belongs
	// with the terramechanics work.
	if (GroundBleedPerSec > 0.f && Owner->IsSupportedByGround())
	{
		StoredMomentumLocal *= FMath::Max(0.f, 1.f - GroundBleedPerSec * DeltaSeconds);
	}
	StoredMomentumLocal = StoredMomentumLocal.BoundToCube(MomentumCapacityNms);
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
	const FVehicleBlockRecord* Record = FindRecord();
	const float Dust = Record ? FMath::Clamp(Record->Condition.SurfaceOpacity01, 0.f, 1.f) : 0.f;
	return FMath::Max(0.f, Def->PowerDelta) * Owner->GetSunFraction() * (1.f - Dust);
}

void UFuelTankModule::Initialize(AVehicleConstruct* InConstruct, int32 InBlockInstanceId)
{
	Super::Initialize(InConstruct, InBlockInstanceId);
}
