// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Net/Serialization/FastArraySerializer.h"
#include "Interfaces/Interactable.h"
#include "Interfaces/Constructible.h"
#include "Interfaces/Pilotable.h"
#include "Interfaces/Damageable.h"
#include "Vehicles/VehicleWheelState.h"
#include "ExoneerTypes.h"
#include "VehicleConstruct.generated.h"

class UVehicleBlockDefinitionDataAsset;
class UVehicleModule;
class UStaticMesh;
class UStaticMeshComponent;
class UInstancedStaticMeshComponent;
class UBoxComponent;
class UInventoryComponent;
class AVehicleConstruct;

/**
 * One placed vehicle block on the construct's 25 cm grid. Replicates through
 * the fast array; clients rebuild collision boxes and mesh instances from
 * these records.
 */
USTRUCT(BlueprintType)
struct FVehicleBlockRecord : public FFastArraySerializerItem
{
	GENERATED_BODY()

	/** Stable per-construct id; never reused. */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle")
	int32 BlockInstanceId = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category = "Vehicle")
	TObjectPtr<UVehicleBlockDefinitionDataAsset> Def = nullptr;

	/** Min-corner cell of the rotated AABB. */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle")
	FIntVector Origin = FIntVector::ZeroValue;

	/** 0..23, see ExoneerVehicleOrientation. */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle")
	uint8 Orientation = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Vehicle")
	int32 StageIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Vehicle")
	float StageProgress01 = 0.f;

	/** Ghost until any progress, Complete when the last stage finishes. */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle")
	EConstructionPhase Phase = EConstructionPhase::Ghost;

	UPROPERTY(BlueprintReadOnly, Category = "Vehicle")
	float Health = 1.f;

	/** Module scratch replicated for VFX: thruster throttle, battery charge 0..1, fuel fill. */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle")
	float StateScalar = 0.f;

	/** Causal condition (tread, dust, temps). Replicated with the record. */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle")
	FPartCondition Condition;

	/**
	 * SERVER-ONLY. Set when deconstruction starts on a Complete block so the
	 * 50% refund penalty holds for the whole reversal; cleared if the block
	 * is welded back to Complete.
	 */
	UPROPERTY(NotReplicated)
	uint8 bDeconstructPenalty = 0;

	/**
	 * CLIENT-SIDE. Fingerprint of everything a visual rebuild reads off this
	 * record (definition, origin, orientation, phase), stamped when the client
	 * last asked for one. Condition replicates on this record too - a heating
	 * winding ticks it about once a second per wheel - and a rebuild recreates
	 * every block's collision box and mesh instance on the construct, so a
	 * reading-only update must not trigger one.
	 */
	UPROPERTY(NotReplicated)
	uint32 VisualKey = 0;

	/**
	 * SERVER-ONLY. Capacity fade earned by this pack but not yet published.
	 * A cycling battery ages by a millionth per tick, so the writer banks it
	 * here and spends it on Condition.CapacityFade01 in deadband steps.
	 */
	UPROPERTY(NotReplicated)
	float PendingCapacityFade = 0.f;

	uint32 ComputeVisualKey() const;

	void PreReplicatedRemove(const struct FVehicleBlockList& InArray);
	void PostReplicatedAdd(const struct FVehicleBlockList& InArray);
	void PostReplicatedChange(const struct FVehicleBlockList& InArray);
};

USTRUCT()
struct FVehicleBlockList : public FFastArraySerializer
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FVehicleBlockRecord> Blocks;

	UPROPERTY(NotReplicated)
	TObjectPtr<AVehicleConstruct> OwnerConstruct = nullptr;

	bool NetDeltaSerialize(FNetDeltaSerializeInfo& DeltaParms)
	{
		return FFastArraySerializer::FastArrayDeltaSerialize<FVehicleBlockRecord, FVehicleBlockList>(Blocks, DeltaParms, *this);
	}
};

template<>
struct TStructOpsTypeTraits<FVehicleBlockList> : public TStructOpsTypeTraitsBase2<FVehicleBlockList>
{
	enum { WithNetDeltaSerializer = true };
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnVehicleBlocksChanged);

/**
 * A vehicle: one server-simulated rigid body plus a unified 25 cm block grid.
 *
 * Physics: the invisible root simulates on the server; every block contributes
 * a welded UBoxComponent with SetMassOverrideInKg(block mass), so total mass,
 * center of mass, and inertia emerge from the layout. Clients do not simulate;
 * they receive replicated movement and rebuild visuals (per-mesh instanced
 * components) and query-only collision from the replicated block records.
 *
 * Power: a simplified ledger over module blocks each server tick produces
 * PowerSupplyFraction, which scales thrust (SE-style brownouts).
 *
 * Piloting: the pilot's CHARACTER stays possessed and forwards its move/look
 * input to the construct through its own Server RPC; the construct routes it
 * to thrust/torque. IPilotable::EnterPilot seats a pawn at a cockpit block.
 *
 * Removing blocks runs split detection: disconnected islands become new
 * constructs (or drop as scrap when trivially small).
 */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API AVehicleConstruct : public AActor, public IInteractable, public IConstructible, public IPilotable, public IDamageable
{
	GENERATED_BODY()

public:
	AVehicleConstruct();

	static constexpr float CellSize = 25.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Vehicle")
	TObjectPtr<UStaticMeshComponent> PhysicsRoot;

	/** Volume cargo on the bed; mass is welded into the rigid body in RebuildDerivedState. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Vehicle")
	TObjectPtr<class UCargoComponent> Cargo;

	// --- Tunables ---
	// (The old mass-scaled RotationTorquePerKg gyro is gone: attitude torque
	// now comes only from installed gyro blocks, which have a rated capacity,
	// a power cost and a momentum budget. GAME-SCOPE section 7 forbids the
	// magic-authority version.)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vehicle") int32 ScrapInsteadOfSplitMaxBlocks = 1;

	/** Server holds the last pilot packet this long; past it, axes zero and the parking brake engages. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vehicle") float PilotInputTimeoutSeconds = 0.5f;

	/**
	 * Suit trickle while seated (W). Registered as a load on the construct
	 * ledger; the suit is charged by that load times the supply fraction.
	 * No O2 from cockpits at alpha.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vehicle") float CockpitSuitChargeW = 300.f;

	/**
	 * Rigid-body damping. Wheel-less constructs keep the legacy values (a
	 * nudged frame should not roll forever). With any Complete wheel, damping
	 * drops to the near-zero floor: Bekker compaction, the Janosi traction
	 * limit, and bearing drag are then the ONLY motion resistance, or the
	 * terramechanics tuning is meaningless.
	 */
	UPROPERTY(EditAnywhere, Category = "Vehicle|Physics") float LinearDampingNoWheels = 0.4f;
	UPROPERTY(EditAnywhere, Category = "Vehicle|Physics") float AngularDampingNoWheels = 2.0f;
	UPROPERTY(EditAnywhere, Category = "Vehicle|Physics") float LinearDampingWheeled = 0.01f;
	UPROPERTY(EditAnywhere, Category = "Vehicle|Physics") float AngularDampingWheeled = 0.05f;

	/**
	 * In flight. Thin-atmosphere drag on a tumbling hull, nothing more.
	 *
	 * Note what these replaced, because it is easy to read them as a loosening
	 * and they are not: damping used to be chosen by wheel presence alone, so a
	 * wheeled rover flew at 0.05 angular and 0.01 linear and the 2.0 angular
	 * value only ever applied to a wheel-less craft. For the rover the owner
	 * actually flies these are a 3x increase in angular and 12x in linear
	 * damping. Angular damping is subtracted from the triad's damping gain (see
	 * ExoneerAttitude::RateGain), so raising it does not double-damp - it moves
	 * work from the gyro to the air. A craft with no gyro block tumbles for a
	 * long time, which is the honest consequence of not fitting attitude
	 * control.
	 */
	UPROPERTY(EditAnywhere, Category = "Vehicle|Physics") float LinearDampingFlying = 0.12f;
	UPROPERTY(EditAnywhere, Category = "Vehicle|Physics") float AngularDampingFlying = 0.15f;

	/**
	 * Keyboard throttle is binary; the drive command ramps toward it so a tap
	 * of W creeps instead of instantly commanding full torque into wheelspin.
	 * Ramp-down is faster so lifting off responds immediately.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vehicle") float DriveThrottleRampUpPerSec = 1.2f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vehicle") float DriveThrottleRampDownPerSec = 4.f;

	/**
	 * Stock motor-controller anti-burnout: torque tapers above this slip
	 * ratio and cuts fully at it. Crude on purpose - the Shear Control talent
	 * later holds the precise 0.15-0.25 optimal window; this only prevents
	 * the runaway spin-to-full-slip dig-in on a held key.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vehicle") float StockSlipCap = 0.85f;

	// --- Replicated state ---
	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Vehicle")
	float PowerSupplyFraction = 1.f;

	UPROPERTY(ReplicatedUsing = OnRep_Pilot, VisibleAnywhere, BlueprintReadOnly, Category = "Vehicle")
	TObjectPtr<APawn> PilotPawn;

	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Vehicle")
	int32 ActiveCockpitId = INDEX_NONE;

	/**
	 * How pilot input is interpreted; toggled by the pilot (V).
	 *
	 * Ground gates THRUST, not the gyro. The triad rate-nulls whenever a pilot
	 * is aboard, in either mode and whether or not a wheel is loaded, which is
	 * the ground question answered on purpose: a reaction wheel really does
	 * resist rotation while the hull sits on its tyres, it is the pilot's
	 * instinctive "make it stop" control, and the wheel-contact bleed in
	 * UGyroModule keeps whatever it stores from being permanent. What Ground
	 * mode does is send W/S and A/D to the wheels instead of the thrusters and
	 * hand the mouse back to the camera, so a rover with thrusters cannot
	 * double-drive. A hybrid defaults to Ground; a wheel-less flyer is snapped
	 * to Flight when it is seated.
	 */
	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Vehicle")
	EPilotControlMode ControlMode = EPilotControlMode::Ground;

	UFUNCTION(BlueprintPure, Category = "Vehicle")
	EPilotControlMode GetControlMode() const { return ControlMode; }

	/** True when the handbrake is held, input timed out, or no pilot is seated. Wheels hold against it. */
	UPROPERTY(Replicated, VisibleInstanceOnly, BlueprintReadOnly, Category = "Vehicle")
	bool bParkingBrakeEngaged = true;

	/**
	 * Per-wheel quantized state, replicated as a side fast array whose
	 * callbacks never MarkVisualsDirty (a spinning wheel must not rebuild the
	 * whole client vehicle). Written by UWheelModule under deadbands; polled
	 * by UpdateWheelVisuals and GetDrivetrainSummary on both sides.
	 */
	UPROPERTY(Replicated)
	FVehicleWheelStateList WheelStates;

	/** SERVER. Find or add the side-array entry for a wheel block. */
	FVehicleWheelStateItem& FindOrAddWheelStateItem(int32 BlockInstanceId);

	/** SERVER. Drop a wheel's side-array entry (block removed/scrapped/moved). */
	void RemoveWheelStateItem(int32 BlockInstanceId);

	/** Aggregate drivetrain readout for instrumentation; safe on both sides. */
	UFUNCTION(BlueprintPure, Category = "Vehicle")
	FVehicleDrivetrainSummary GetDrivetrainSummary() const;

	/** True while any wheel is compressed. */
	bool IsAnyWheelInContact() const;

	/**
	 * True while the craft is supported by something outside itself: a loaded
	 * tyre, or a hull resting on the ground. The gyro's momentum sink reads
	 * THIS, not the wheel predicate, because the reaction torque a reaction
	 * wheel unwinds against comes from the contact and not from the tyre: a
	 * craft built with no wheel blocks has an empty wheel array and used to
	 * have no momentum sink at all, so one saturated axis was permanent.
	 *
	 * Wheels answer it for free. Without them the hull's own footprint is swept
	 * a short way down; the probe distance is a fraction of the build grid's
	 * cell, not an authored tuning value.
	 */
	bool IsSupportedByGround() const;

	/**
	 * Hull inertia about the centre of mass in body axes (kg*m^2); zero until
	 * there is a body. Public because UGyroModule needs it to advance the
	 * gyroscopic cross term to the end of the step - see its comment on why
	 * the start-of-step form adds energy to an abandoned craft.
	 */
	FVector GetBodyInertiaKgM2() const;

private:
	/**
	 * One-frame memo for IsSupportedByGround. The predicate runs a
	 * full-footprint box sweep and three callers want it every tick (the
	 * construct's own tick plus one per gyro), while its cheap reject only
	 * fires when the hull is falling faster than it could settle - so hover,
	 * climb and level flight all paid for three sweeps. The answer cannot
	 * change inside one frame, so it is computed at most once.
	 */
	mutable uint64 GroundSupportFrame = 0;
	mutable bool bGroundSupportCached = false;

public:

	/** Any Complete block whose definition is a wheel. */
	UFUNCTION(BlueprintPure, Category = "Vehicle")
	bool HasCompleteWheel() const;

	/** Any Complete block whose module is a thruster (the craft can fly). */
	UFUNCTION(BlueprintPure, Category = "Vehicle")
	bool HasCompleteThruster() const;

	/** Total installed reaction torque (N*m) over Complete gyro blocks. Record-driven, so valid on clients too. */
	UFUNCTION(BlueprintPure, Category = "Vehicle")
	float GetInstalledGyroTorqueNm() const;

	/**
	 * Total installed rotor momentum capacity per axis (N*m*s) over Complete
	 * gyro blocks. The visor divides the untrimmed standing moment into it to
	 * get the seconds the pitch axis has left, so it has to be readable on the
	 * client too - hence record-driven, like the torque above.
	 */
	UFUNCTION(BlueprintPure, Category = "Vehicle")
	float GetInstalledGyroMomentumCapacityNms() const;

	/** Persistent per-wheel settings queued by the save-load path until the module spawns. */
	struct FWheelSavedState
	{
		float TirePressureKPa = 0.f;   // 0 = use the authored nominal
		float SteerTrimDeg = 0.f;
	};

	/** SERVER. Queue saved wheel settings; UWheelModule::Initialize consumes them. */
	void QueueWheelStateRestore(int32 BlockInstanceId, float TirePressureKPa, float SteerTrimDeg);

	/** SERVER. Consume-and-remove queued settings for a block. False if none pending. */
	bool TakeSavedWheelState(int32 BlockInstanceId, FWheelSavedState& OutState);

	/** SERVER. Read a live wheel module's persistent settings for the save path. False if not a live wheel. */
	bool GetWheelPersistentState(int32 BlockInstanceId, float& OutTirePressureKPa, float& OutSteerTrimDeg) const;

	UPROPERTY(BlueprintAssignable) FOnVehicleBlocksChanged OnBlocksChanged;

	// --- SERVER grid API (called from build tool RPCs and modules) ---

	UFUNCTION(BlueprintCallable, Category = "Vehicle")
	bool CanPlaceBlock(UVehicleBlockDefinitionDataAsset* Def, FIntVector Origin, uint8 Orientation, EBuildPlacementError& OutError) const;

	/** Spawn a GHOST block record. Returns its BlockInstanceId or INDEX_NONE. */
	UFUNCTION(BlueprintCallable, Category = "Vehicle")
	int32 PlaceBlockGhost(UVehicleBlockDefinitionDataAsset* Def, FIntVector Origin, uint8 Orientation);

	UFUNCTION(BlueprintCallable, Category = "Vehicle")
	bool RemoveBlock(int32 BlockInstanceId);

	UFUNCTION(BlueprintCallable, Category = "Vehicle")
	float ApplyDamageToBlockAt(const FVector& WorldPoint, float Amount, EExoneerDamageType Type, AActor* DamageInstigator);

	/**
	 * SERVER. Replace the part at WorldPoint if a spare is in Source
	 * (wheels consume a tire). Resets that record's condition.
	 */
	UFUNCTION(BlueprintCallable, Category = "Vehicle")
	bool ReplacePartAt(const FVector& WorldPoint, UInventoryComponent* Source);

	/** SERVER. Wipe solar/radio dust on the block at WorldPoint. */
	UFUNCTION(BlueprintCallable, Category = "Vehicle")
	bool WipePartAt(const FVector& WorldPoint);

	UFUNCTION(BlueprintPure, Category = "Vehicle")
	float GetStoredFuelKg() const;

	UFUNCTION(BlueprintPure, Category = "Vehicle")
	bool HasFuelCapacity() const;

	/** SERVER. Draw fuel from tank records. False if the tanks cannot cover Kg. */
	bool ConsumeFuelKg(float Kg);

	/** Wet mass TWR using installed thruster Newtons / (mass * |g|). 0 if no physics body. */
	UFUNCTION(BlueprintPure, Category = "Vehicle")
	float GetAscentTwr() const;

	UFUNCTION(BlueprintPure, Category = "Vehicle")
	float GetMinTreadDepthMm() const;

	/** Hottest Complete wheel winding (C). Returns -1000 when no wheel block is installed. */
	UFUNCTION(BlueprintPure, Category = "Vehicle")
	float GetMaxWindingTempC() const;

	/** True while any Complete wheel holds its over-temp cutout (that motor makes no torque). */
	UFUNCTION(BlueprintPure, Category = "Vehicle")
	bool HasThermalCutout() const;

	/** True while any Complete wheel sits past its derate onset temperature. */
	UFUNCTION(BlueprintPure, Category = "Vehicle")
	bool IsAnyWindingDerating() const;

	void GetBatteryEnergy(float& OutStoredWs, float& OutCapacityWs) const;

	void InitializeRecordCondition(FVehicleBlockRecord& Record);
	bool RestoreBlockCondition(int32 BlockInstanceId, const FPartCondition& Condition);
	void MarkRecordDirty(FVehicleBlockRecord& Record);
	void ApplyWeatherWear(float StormIntensity, float DtSeconds);

	/** SERVER. Found a brand-new construct with its first ghost block (at the grid origin, with the given orientation). */
	static AVehicleConstruct* FoundConstruct(UWorld* World, UVehicleBlockDefinitionDataAsset* Def, const FTransform& Transform, EBuildPlacementError& OutError, uint8 Orientation = 0);

	// --- Queries (server + client) ---
	UFUNCTION(BlueprintPure, Category = "Vehicle") int32 GetBlockCount() const { return BlockList.Blocks.Num(); }
	UFUNCTION(BlueprintPure, Category = "Vehicle") const TArray<FVehicleBlockRecord>& GetBlocks() const { return BlockList.Blocks; }
	const FVehicleBlockRecord* FindRecord(int32 BlockInstanceId) const;
	FVehicleBlockRecord* FindMutableRecord(int32 BlockInstanceId);
	int32 FindBlockAtCell(const FIntVector& Cell) const;
	int32 FindBlockAtWorldPoint(const FVector& WorldPoint) const;

	UFUNCTION(BlueprintPure, Category = "Vehicle")
	FIntVector WorldToCell(const FVector& WorldLocation) const;

	UFUNCTION(BlueprintPure, Category = "Vehicle")
	FVector CellToWorld(const FIntVector& Cell) const;

	UFUNCTION(BlueprintPure, Category = "Vehicle")
	FTransform GetBlockWorldTransform(const FVehicleBlockRecord& Record) const;

	/** Block transform in construct-local space: orientation quat at the AABB center. */
	FTransform GetBlockLocalTransform(const FVehicleBlockRecord& Record) const;

	/** Sun fraction pull-through for solar modules (0 when no env manager). */
	float GetSunFraction() const;

	/** Cached world environment manager (biome default soil, weather); may be null. */
	const class APlanetEnvironmentManager* GetEnvironmentManager() const;

	/** SERVER. Pilot input packet, forwarded by the pilot's character RPC. Held until the next packet or timeout. */
	void SetPilotInput(const FPilotInput& Input);

	/**
	 * SERVER. Overwrite one record's construction/health fields (save-load
	 * path) and mark it dirty. OrientationOverride >= 0 also restores the
	 * orientation (used for the founding block). Returns false if the id is
	 * unknown.
	 */
	bool RestoreBlockRecord(int32 BlockInstanceId, EConstructionPhase InPhase, int32 InStageIndex, float InStageProgress01, float InHealth, float InStateScalar, int32 OrientationOverride = -1);

	// IPilotable
	virtual bool EnterPilot_Implementation(APawn* Pilot, int32 StationId) override;
	virtual void ExitPilot_Implementation(APawn* Pilot) override;
	virtual void ApplyPilotInput_Implementation(const FPilotInput& Input) override;

	// IInteractable (interact = enter/exit nearest cockpit)
	virtual bool OnInteract_Implementation(AActor* Interactor) override;
	virtual FText GetInteractionPrompt_Implementation() const override;
	virtual FGameplayTagContainer GetInteractionTags_Implementation() const override;

	// IConstructible (per-block, addressed by WorldPoint)
	virtual EConstructionPhase GetConstructionPhaseAt_Implementation(const FVector& WorldPoint) const override;
	virtual float GetConstructionProgressAt_Implementation(const FVector& WorldPoint) const override;
	virtual float GetConstructionProgressForTarget_Implementation(int32 TargetId) const override;
	virtual int32 GetConstructionTargetIdAt_Implementation(const FVector& WorldPoint) const override;
	virtual float InvestConstruction_Implementation(AActor* Builder, UInventoryComponent* SourceInventory, const FVector& WorldPoint, float WeldPoints, int32& OutTargetId) override;
	virtual float DeconstructAt_Implementation(AActor* Builder, UInventoryComponent* RefundInventory, const FVector& WorldPoint, float WreckPoints) override;

	// IDamageable (routes to the hit block)
	virtual float ApplyExoneerDamage_Implementation(float Amount, EExoneerDamageType Type, AActor* Instigator) override;

	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Rebuild boxes/instances/cell map from records. Client fast array hook. */
	void MarkVisualsDirty() { bVisualsDirty = true; }

protected:
	virtual void BeginPlay() override;

	UPROPERTY(Replicated)
	FVehicleBlockList BlockList;

	/** SERVER. Live modules by BlockInstanceId (Complete functional blocks only). */
	UPROPERTY()
	TMap<int32, TObjectPtr<UVehicleModule>> Modules;

	/** Rebuilt from records on both sides. */
	TMap<FIntVector, int32> CellToBlock;
	UPROPERTY() TMap<int32, TObjectPtr<UBoxComponent>> BlockBodies;
	UPROPERTY() TMap<TObjectPtr<UStaticMesh>, TObjectPtr<UInstancedStaticMeshComponent>> VisualLayers;

	/**
	 * Dedicated animated mesh per Complete wheel block (both sides). Wheels
	 * are excluded from the ISMC layers: instance indices there are discarded
	 * and every replicated record change rebuilds all instances, so an ISMC
	 * instance can neither spin nor steer.
	 */
	UPROPERTY() TMap<int32, TObjectPtr<UStaticMeshComponent>> WheelVisuals;

	/** Locally integrated wheel spin angles (rad), keyed by BlockInstanceId. Cosmetic, never replicated. */
	TMap<int32, float> WheelSpinAngles;

	int32 NextBlockInstanceId = 0;
	bool bVisualsDirty = false;

	/** SERVER. Last received pilot packet, held until superseded or timed out. */
	FPilotInput PilotInput;

	/** SERVER. World seconds of the last received packet; < 0 = none yet. */
	double LastPilotInputServerTime = -1.0;

	/** SERVER. Rolling-counter bookkeeping for the mode toggle. */
	uint8 LastProcessedModeToggle = 0;
	bool bModeToggleSyncPending = true;

	/** SERVER. Ramped drive throttle (see DriveThrottleRamp* tunables). */
	float CurrentDriveThrottle = 0.f;

	/**
	 * Lift valve setting, 0..1, replicated so the visor can show it. It FOLLOWS
	 * a target at the thruster's authored slew rate in both directions, and the
	 * target is three-state: the reserved ceiling while the lift key is held,
	 * zero while the descend key is held, and the HOVER setting when neither is
	 * held and the craft is airborne under a live pilot.
	 *
	 * Nothing latches. Every path that ends the pilot's authority drives the
	 * target to zero: a wheel back on the ground, an input timeout, leaving the
	 * seat, switching to Ground mode - and the descend key closes it at any
	 * time. The hover state is a governor with a rate term and no integrator,
	 * so its whole state is a vertical speed the pilot can read off the visor.
	 *
	 * The rate limit stays because a valve cannot step from zero to full thrust
	 * in one frame; splitting the collective out of the horizontal intent stays
	 * too, because normalising all three axes together stole lift the moment
	 * the pilot also asked to go forward.
	 */
	UPROPERTY(Replicated)
	float LiftCollective = 0.f;

	/**
	 * True while the lift governor is holding altitude rather than the pilot
	 * holding a key. Replicated purely so the visor can label it: a valve that
	 * is open because a computer decided so, and not because a finger is down,
	 * has to say which it is.
	 */
	UPROPERTY(Replicated)
	bool bLiftGovernorActive = false;

	/**
	 * True while the governor has run out of bank angle: the reserved ceiling
	 * cannot make weight at this attitude, so the valve is FROZEN where the
	 * pilot last had it and the craft is sinking. Replicated because HOVER and
	 * PINNED are different machine states and the visor used to print HOVER
	 * for both - while the craft sank and accelerated sideways.
	 */
	UPROPERTY(Replicated)
	bool bLiftGovernorPinned = false;

	/** True while the governor is flying the descend key's bounded rate. */
	UPROPERTY(Replicated)
	bool bLiftDescending = false;

	/**
	 * True while the craft's lift has NO upward component left, so the governor
	 * has shut the valve: not "sinking with the valve where you left it", which
	 * is PINNED, but "falling with the engine off because the engine is
	 * pointing at the ground". A different state and a different word, because
	 * the pilot's way out is different - roll back, do not add throttle.
	 */
	UPROPERTY(Replicated)
	bool bLiftInverted = false;

	/**
	 * Net LATERAL force the pilot's own base throttles make, along the seat's
	 * right axis (N). Signed; positive is to the pilot's right.
	 *
	 * Flight has no lateral thrust command at all - Input_Move writes Move.X
	 * and Rotate.Z and never Move.Y - so any standing side force is one nobody
	 * asked for and nothing can answer. It is also invisible in every other
	 * readout: the trim path nulls the net TRIM force and never the base
	 * throttle's force, so a build whose lift nozzles are all toed the same way
	 * drifts sideways at 1904 N (1.03 m/s^2) for ever with a clean momentum
	 * store and a green visor. That is exactly what a player gets from the aim
	 * list if he picks one toe direction six times, so the visor has to say it.
	 *
	 * Low-passed like the untrimmed moment, for the same reason: the number the
	 * pilot needs is the sustained bias, not the valve slewing.
	 */
	UPROPERTY(Replicated)
	float StandingSideForceN = 0.f;

	/** Which body axis carries the worst untrimmed moment: 0 roll, 1 pitch, 2 yaw. */
	UPROPERTY(Replicated)
	uint8 UntrimmedWorstAxis = 0;

	/**
	 * Worst-axis standing moment the thrust group could NOT cancel (N*m), i.e.
	 * what the rotors are paying for out of their momentum store, in N*m*s per
	 * second. Zero on a balanced build. Replicated so the visor can turn it
	 * into the seconds-to-saturation countdown it really is - the honest answer
	 * to "why did my craft suddenly stop responding in pitch".
	 */
	UPROPERTY(Replicated)
	float UntrimmedStandingMomentNm = 0.f;

	/**
	 * SERVER. Attitude reference the roll/pitch hold tracks. World frame.
	 *
	 * It holds ROLL AND PITCH ONLY - yaw is released every frame, so heading
	 * stays on pure rate - and it slews back to LEVEL when the stick is
	 * released. That is the way out of an arbitrary bank, which a thrust
	 * vehicle otherwise never gets: with no restoring moment about its own
	 * centre of mass, a 20 degree bump is a permanent 3.6 m/s^2 sideways
	 * acceleration.
	 *
	 * The freeze the earlier pass wanted is a DROP: the reference is invalid
	 * whenever the pilot is not flying an airborne craft, and re-seeded from
	 * the hull attitude when he is. A reference that does not exist cannot
	 * integrate while the suspension holds the hull and then release a banked
	 * error the instant the wheels leave the ground.
	 */
	FQuat AttitudeReference = FQuat::Identity;
	bool bAttitudeReferenceValid = false;

	/**
	 * SERVER. Low-passed attitude command, body axes (N*m): the part of the
	 * loop's torque that does NOT decay and therefore must not be paid for out
	 * of rotor momentum. Handed to the differential-thrust trim each tick. See
	 * ExoneerThrust.h rule 2b - holding a rate against hull damping costs
	 * D*I*w every second, for ever, and that is what took the yaw axis out.
	 */
	FVector SustainedAttitudeTorqueLocal = FVector::ZeroVector;

	/**
	 * SERVER. Latch on the momentum dump, so an unwind runs down to the
	 * authored release fraction instead of stopping the moment saturation dips
	 * back under the onset and parking the axis there.
	 */
	bool bDesaturatingLatched = false;

	/** Peak downward speed (UU/s) while no wheel is in contact. */
	float AirborneDownSpeedUU = 0.f;

	/**
	 * SERVER. Two-edge debounce on the ground/air decision. The latch flips to
	 * airborne only after AttitudeGroundReleaseSeconds clear of the ground, and
	 * back to grounded only after the same time IN contact, so a rover bouncing
	 * over rough terrain settles on one answer. A one-edge debounce was the
	 * bug: the release edge was debounced and the contact edge was not, so a
	 * single wheel tap flipped the state instantly.
	 *
	 * What the latch gates is the LIFT GOVERNOR - hover hold must not open the
	 * valve on a craft sitting on its wheels, and one wheel tap must not slam
	 * it shut mid-flight. The attitude loop is NOT gated on it; see
	 * EPilotControlMode.
	 */
	bool bGroundContactLatched = true;
	float GroundStateTimerSeconds = 0.f;

	/**
	 * SERVER. True when the last pilot packet is older than the timeout, or no
	 * pilot is aboard. The lift governor requires a live pilot: a released key
	 * and a dead connection look identical in the input packet, and only one of
	 * them should leave the valve holding altitude.
	 */
	bool bPilotInputStale = true;

	UPROPERTY(Replicated)
	float LastLandingSpeedMS = 0.f;

	/**
	 * Worst-axis fraction of the gyro momentum envelope in use, 0..1. Written
	 * by the server attitude loop and replicated purely so the visor can show
	 * it: a pilot whose attitude authority is about to vanish must be able to
	 * see it coming.
	 */
	UPROPERTY(Replicated)
	float GyroSaturation01 = 0.f;

	/** Body axis carrying the worst rotor store: 0 roll, 1 pitch, 2 yaw. */
	UPROPERTY(Replicated)
	uint8 GyroWorstAxis = 0;

public:
	/**
	 * SERVER. Set by the save-load replay to restore parked geometry
	 * byte-exact (a rover saved with wheels in soft soil must not lose them
	 * to the overlap rejection on load). Cleared after the replay.
	 */
	bool bSuppressWorldOverlapCheck = false;

protected:

	UFUNCTION() void OnRep_Pilot();

	/**
	 * SERVER. One Chaos sim callback for all this construct's wheels, running
	 * the terramechanics per internal solver substep. Registered on demand in
	 * MarshalWheelPhysics; freed in EndPlay through the solver (never deleted
	 * directly - the solver owns the lifetime after unregister).
	 */
	class FExoneerWheelSimCallback* WheelSimCallback = nullptr;

	/** SERVER. Pending saved wheel settings, keyed by BlockInstanceId. */
	TMap<int32, FWheelSavedState> PendingWheelRestore;

	// --- Internals ---
	void RebuildDerivedState();          // cell map + bodies + visuals + mass
	void ServerTickPowerLedger(float DeltaSeconds);
	void ServerTickModules(float DeltaSeconds);
	void ServerRouteThrust(float DeltaSeconds);
	void ServerRouteDrive(float DeltaSeconds);   // ground mode: throttle/steer/brake to wheel modules (Ackermann)
	void UpdateWheelVisuals(float DeltaSeconds); // both sides: pose wheel meshes from WheelStates
	void DrainWheelTelemetry();          // server: pop physics outputs into wheel modules (pre-ledger)
	void MarshalWheelPhysics();          // server: build this frame's sim-callback input (post module tick)
	void SyncModulesToRecords();         // create/destroy modules on phase changes
	void RunSplitDetection();            // after any removal
};
