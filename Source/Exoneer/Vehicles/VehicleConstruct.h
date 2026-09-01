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

	/** Module scratch replicated for VFX: thruster throttle, battery charge 0..1. */
	UPROPERTY(BlueprintReadOnly, Category = "Vehicle")
	float StateScalar = 0.f;

	/**
	 * SERVER-ONLY. Set when deconstruction starts on a Complete block so the
	 * 50% refund penalty holds for the whole reversal; cleared if the block
	 * is welded back to Complete.
	 */
	UPROPERTY(NotReplicated)
	uint8 bDeconstructPenalty = 0;

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

	// --- Tunables ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vehicle") float RotationTorquePerKg = 800.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vehicle") int32 ScrapInsteadOfSplitMaxBlocks = 1;

	/** Server holds the last pilot packet this long; past it, axes zero and the parking brake engages. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vehicle") float PilotInputTimeoutSeconds = 0.5f;

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
	 * Gyro torque multiplier while in Ground control mode. 0 by default: a
	 * rover in the air is ballistic; free attitude authority is a Flight-mode
	 * (thruster craft) legacy, flagged in the scope gap map.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vehicle") float GroundModeGyroFraction = 0.f;

	// --- Replicated state ---
	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Vehicle")
	float PowerSupplyFraction = 1.f;

	UPROPERTY(ReplicatedUsing = OnRep_Pilot, VisibleAnywhere, BlueprintReadOnly, Category = "Vehicle")
	TObjectPtr<APawn> PilotPawn;

	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Vehicle")
	int32 ActiveCockpitId = INDEX_NONE;

	/** How pilot input is interpreted; toggled by the pilot (V). Ground gates the gyro. */
	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Vehicle")
	EPilotControlMode ControlMode = EPilotControlMode::Flight;

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

	/** Any Complete block whose definition is a wheel. */
	UFUNCTION(BlueprintPure, Category = "Vehicle")
	bool HasCompleteWheel() const;

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

	/** SERVER. Restore up to Amount health on the block at WorldPoint (weld-to-repair). Returns health restored. */
	UFUNCTION(BlueprintCallable, Category = "Vehicle")
	float RepairBlockAt(const FVector& WorldPoint, float Amount);

	/** SERVER. Found a brand-new construct with its first ghost block (at the grid origin, with the given orientation). */
	static AVehicleConstruct* FoundConstruct(UWorld* World, UVehicleBlockDefinitionDataAsset* Def, const FTransform& Transform, EBuildPlacementError& OutError, uint8 Orientation = 0);

	// --- Queries (server + client) ---
	UFUNCTION(BlueprintPure, Category = "Vehicle") int32 GetBlockCount() const { return BlockList.Blocks.Num(); }
	UFUNCTION(BlueprintPure, Category = "Vehicle") const TArray<FVehicleBlockRecord>& GetBlocks() const { return BlockList.Blocks; }
	const FVehicleBlockRecord* FindRecord(int32 BlockInstanceId) const;
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
	virtual float InvestConstruction_Implementation(AActor* Builder, UInventoryComponent* SourceInventory, const FVector& WorldPoint, float WeldPoints) override;
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
	FVehicleBlockRecord* FindMutableRecord(int32 BlockInstanceId);
	void MarkRecordDirty(FVehicleBlockRecord& Record);
};
