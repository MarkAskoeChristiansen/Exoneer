// Copyright Exoneer contributors.
#include "Vehicles/VehicleModule.h"
#include "Vehicles/VehicleConstruct.h"
#include "Data/VehicleBlockDefinitionDataAsset.h"
#include "Interfaces/Damageable.h"
#include "Interfaces/Pilotable.h"
#include "Vehicles/ExoneerVehicleUnits.h"
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
