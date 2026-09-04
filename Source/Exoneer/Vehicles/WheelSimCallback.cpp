// Copyright Exoneer contributors.
#include "Vehicles/WheelSimCallback.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "Chaos/Particle/ParticleUtilities.h"

void FExoneerWheelSimCallback::OnPreSimulate_Internal()
{
	const FExoneerWheelSimInput* Input = GetConsumerInput_Internal();
	if (!Input || !Input->Proxy || Input->Wheels.Num() == 0)
	{
		return;
	}

	// The internal handle may already be gone (body destroyed mid-frame) and
	// the body must be dynamic for accumulated forces to mean anything.
	Chaos::FRigidBodyHandle_Internal* Handle = Input->Proxy->GetPhysicsThreadAPI();
	if (!Handle || Handle->ObjectState() != Chaos::EObjectStateType::Dynamic)
	{
		return;
	}

	const float Dt = (float)GetDeltaTime_Internal();
	if (Dt <= 0.f)
	{
		return;
	}

	ExoneerWheelSim::FWheelBodyView Body;
	Body.BodyTM = FTransform(Handle->GetR(), Handle->GetX());
	Body.LinearVelocityUU = FVector(Handle->V());
	Body.AngularVelocityRad = FVector(Handle->W());
	Body.ComWorldUU = FVector(Chaos::FParticleUtilitiesGT::GetCoMWorldPosition(Handle));
	Body.MassKg = (float)Handle->M();
	Body.WheelCount = Input->Wheels.Num();
	Body.ComRotation = FQuat(Chaos::FParticleUtilitiesGT::GetCoMWorldRotation(Handle));
	Body.InvInertiaDiag = FVector(Handle->InvI());

	FExoneerWheelSimOutput& Output = GetProducerOutputData_Internal();
	Output.Wheels.SetNum(Input->Wheels.Num());

	for (int32 Index = 0; Index < Input->Wheels.Num(); ++Index)
	{
		const ExoneerWheelSim::FWheelSimInputItem& Item = Input->Wheels[Index];
		ExoneerWheelSim::FWheelSimState& State = WheelStates_Internal.FindOrAdd(Item.BlockInstanceId);

		const ExoneerWheelSim::FWheelSimForce Force =
			ExoneerWheelSim::StepWheel(Dt, Item, Body, State, Output.Wheels[Index]);

		if (Force.bApply)
		{
			// Accumulators are zeroed every step, so re-apply each substep.
			// bInvalidate=false keeps sleeping bodies asleep; the game thread
			// wakes the body explicitly when drive input arrives.
			const Chaos::FVec3 ChaosForce(Force.ForceUE);
			Handle->AddForce(ChaosForce, /*bInvalidate*/ false);
			Handle->AddTorque(Chaos::FVec3::CrossProduct(Chaos::FVec3(Force.LocationUU) - Chaos::FVec3(Body.ComWorldUU), ChaosForce), /*bInvalidate*/ false);
		}
	}

	// Drop integrator state for wheels no longer marshaled (removed/split).
	if (WheelStates_Internal.Num() > Input->Wheels.Num())
	{
		TSet<int32, DefaultKeyFuncs<int32>, TInlineSetAllocator<16>> LiveIds;
		for (const ExoneerWheelSim::FWheelSimInputItem& Item : Input->Wheels)
		{
			LiveIds.Add(Item.BlockInstanceId);
		}
		for (auto It = WheelStates_Internal.CreateIterator(); It; ++It)
		{
			if (!LiveIds.Contains(It->Key))
			{
				It.RemoveCurrent();
			}
		}
	}
}
