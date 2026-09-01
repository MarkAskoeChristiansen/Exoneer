// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Chaos/SimCallbackObject.h"
#include "Vehicles/WheelSimTypes.h"

/**
 * One construct-owned Chaos sim callback for ALL its wheels. Registered by
 * AVehicleConstruct on demand and freed in EndPlay via
 * UnregisterAndFreeSimCallbackObject_External (never deleted directly - the
 * solver owns the lifetime after unregister).
 *
 * OnPreSimulate_Internal fires once per internal solver step (a true substep
 * when bSubstepping splits the frame), so the suspension spring-damper and
 * the slip feedback loop integrate at fixed dt regardless of frame rate -
 * the whole point of moving off the actor tick.
 *
 * Threading: the input struct is immutable per-frame marshaled data (game
 * thread produces in AVehicleConstruct::MarshalWheelPhysics, one input is
 * shared by every substep of the frame). Persistent integrator state (wheel
 * omega, one-substep lags) lives HERE as callback members, never in the
 * input. No UObject is touched on the physics thread.
 */
struct FExoneerWheelSimInput : public Chaos::FSimCallbackInput
{
	/** Refreshed from FBodyInstance::GetPhysicsActor() EVERY game frame - never cached. */
	Chaos::FSingleParticlePhysicsProxy* Proxy = nullptr;

	TArray<ExoneerWheelSim::FWheelSimInputItem> Wheels;

	void Reset()
	{
		Proxy = nullptr;
		Wheels.Reset();   // keeps capacity (inputs are pooled and recycled)
	}
};

struct FExoneerWheelSimOutput : public Chaos::FSimCallbackOutput
{
	TArray<ExoneerWheelSim::FWheelSimTelemetry> Wheels;

	void Reset()
	{
		Wheels.Reset();
	}
};

class FExoneerWheelSimCallback final
	: public Chaos::TSimCallbackObject<FExoneerWheelSimInput, FExoneerWheelSimOutput, Chaos::ESimCallbackOptions::Presimulate>
{
public:
	virtual FName GetFNameForStatId() const override
	{
		static const FLazyName StaticName("ExoneerWheelSimCallback");
		return StaticName;
	}

private:
	virtual void OnPreSimulate_Internal() override;

	/** Physics-thread persistent per-wheel state, keyed by BlockInstanceId. */
	TMap<int32, ExoneerWheelSim::FWheelSimState> WheelStates_Internal;
};
