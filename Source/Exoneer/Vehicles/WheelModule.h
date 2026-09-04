// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Vehicles/VehicleModule.h"
#include "Vehicles/VehicleWheelSpec.h"
#include "Vehicles/WheelSimTypes.h"
#include "WheelModule.generated.h"

class ABasePiece;

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

	/**
	 * Consume one physics-step telemetry packet: ledger cache plus this frame's
	 * condition accumulators. The construct pops one packet per physics
	 * SUBSTEP, so nothing is integrated here - TickModule does that once per
	 * frame (see IntegrateCondition).
	 */
	void ConsumeTelemetry(const ExoneerWheelSim::FWheelSimTelemetry& Telemetry);

private:
	const FVehicleWheelSpec* FindSpec() const;
	ExoneerTerramechanics::FSoilParams ResolveSoil(const FHitResult& Hit) const;

	/** SERVER. Spend tread and step the winding for one frame, then deadband the record. */
	void IntegrateCondition(FVehicleBlockRecord& Record, const FVehicleWheelSpec& Spec, float DeltaSeconds);

	/** SERVER. Push this frame's mean contact load onto the piece under the wheel. */
	void ReportGroundLoad();

	/**
	 * The base piece the suspension probe resolved as ground this frame, or
	 * null for terrain. GAME THREAD ONLY and never marshaled: the physics
	 * structs carry plain geometry and soil, no UObject. Weak, because a deck
	 * collapsing under the wheel must read back as null and not as a stale
	 * pointer into a destroyed actor.
	 */
	TWeakObjectPtr<ABasePiece> GroundPiece;

	float TirePressureKPa = 0.f;        // 0 until Initialize resolves saved or nominal
	float SteerTrimRad = 0.f;
	float SteerAngleRad = 0.f;
	float WheelInertiaKgM2 = 1.f;

	/** Per-frame ground cache, rebuilt by TickModule before marshaling. */
	ExoneerWheelSim::FWheelSimGround Ground;
	bool bGroundCacheValid = false;

	/**
	 * Seconds of contact grace left on the cached plane. A round wheel on a
	 * continuous surface does not lose and regain the ground at frame rate;
	 * only the SAMPLING is discrete. When a probe misses, the last plane
	 * stands for this long and the strut still decides contact - a plane out
	 * of the strut's reach reads airborne exactly as before - so nothing is
	 * restored that the geometry denies.
	 */
	float ContactGraceRemaining = 0.f;

	/** Last physics-step telemetry (ledger + replication source). */
	ExoneerWheelSim::FWheelSimTelemetry LastTelemetry;

	/**
	 * This frame's telemetry, accumulated by ConsumeTelemetry and integrated
	 * once by TickModule. Integrating per packet ran wear and heat once per
	 * PHYSICS SUBSTEP against a frame dt (about 2x real time at 120 Hz
	 * substeps and 60 fps, and frame-rate dependent), and a sleeping body
	 * sends no packets at all - which stalled the winding, so a tripped motor
	 * never cooled and never cleared its latch.
	 */
	float FrameLossWSum = 0.f;
	float FrameSlipSum = 0.f;
	float FrameLoadNSum = 0.f;
	/** Frictional power in the contact patch (W): shear force times sliding speed. */
	float FrameShearPowerWSum = 0.f;
	int32 FrameTelemetryCount = 0;
	int32 FrameContactCount = 0;

	/**
	 * Record values at the last dirty mark, so the 0.1 mm and 1 C deadbands
	 * measure ACCUMULATED change. Measuring this frame's delta instead only
	 * ever saw about 0.1 C at the stall heating rate, so the readings never
	 * replicated after the initial baseline and a client's pilot panel sat at
	 * ambient while the server cooked the motor.
	 */
	float LastMarkedTreadDepthMm = 0.f;
	float LastMarkedWindingTempC = 0.f;

	bool bShutDown = false;
};
