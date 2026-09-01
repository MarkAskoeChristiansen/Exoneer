// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Vehicles/VehicleModule.h"
#include "Vehicles/VehicleWheelSpec.h"
#include "Vehicles/WheelSimTypes.h"
#include "WheelModule.generated.h"

/**
 * Server-side behavior of one wheel block. The game-thread half: slews the
 * steering servo and the CTIS valve, runs the per-frame suspension probe
 * (scene queries are forbidden on the physics thread), builds the marshaled
 * input for the construct's Chaos sim callback, and publishes telemetry into
 * the replicated side array + the power ledger. The per-substep physics runs
 * in Vehicles/WheelSimStep.cpp via FExoneerWheelSimCallback.
 */
UCLASS()
class EXONEER_API UWheelModule : public UVehicleModule
{
	GENERATED_BODY()

public:
	virtual void Initialize(AVehicleConstruct* InConstruct, int32 InBlockInstanceId) override;
	virtual void Shutdown() override;
	virtual void TickModule(float DeltaSeconds) override;
	virtual float GetCurrentDraw() const override;

	// --- Commands, set by AVehicleConstruct::ServerRouteDrive each tick ---
	float ThrottleCommand = 0.f;        // -1..1, already gated by bDriven
	float BrakeCommand = 0.f;           // 0..1 service brake
	bool bParkingBrake = true;
	float TargetSteerAngleRad = 0.f;    // Ackermann-resolved by the construct
	float TargetSlipCap = 1.f;          // Shear Control talent lever

	// --- Persistent settings (save/load) ---
	void RestorePersistentState(float InTirePressureKPa, float InSteerTrimDeg);
	float GetTirePressureKPa() const { return TirePressureKPa; }
	float GetSteerTrimDeg() const { return FMath::RadiansToDegrees(SteerTrimRad); }
	float GetSteerAngleRad() const { return SteerAngleRad; }

	/** Fill this frame's marshaled input. False when the wheel cannot simulate (no record/def). */
	bool BuildSimInput(ExoneerWheelSim::FWheelSimInputItem& OutItem) const;

	/** Consume one physics-step telemetry packet: side array (deadbanded) + ledger cache. */
	void ConsumeTelemetry(const ExoneerWheelSim::FWheelSimTelemetry& Telemetry);

private:
	const FVehicleWheelSpec* FindSpec() const;
	ExoneerTerramechanics::FSoilParams ResolveSoil(const FHitResult& Hit) const;

	float TirePressureKPa = 0.f;        // 0 until Initialize resolves saved or nominal
	float SteerTrimRad = 0.f;
	float SteerAngleRad = 0.f;
	float WheelInertiaKgM2 = 1.f;

	/** Per-frame ground cache, rebuilt by TickModule before marshaling. */
	ExoneerWheelSim::FWheelSimGround Ground;
	bool bGroundCacheValid = false;

	/** Last physics-step telemetry (ledger + replication source). */
	ExoneerWheelSim::FWheelSimTelemetry LastTelemetry;

	bool bShutDown = false;
};
