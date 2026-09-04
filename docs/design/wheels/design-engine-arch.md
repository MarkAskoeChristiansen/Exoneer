# Exoneer wheel pass – C++ architecture for Bekker-Wong terramechanics on AVehicleConstruct

ChaosVehiclesPlugin stays unused: its `UChaosWheeledVehicleMovementComponent` assumes an authored skeletal mesh with wheel bones on a single pawn, which cannot follow a runtime-assembled box-weld body whose wheel count changes every `RebuildDerivedState`; everything below is custom on the existing construct.

Conventions used throughout: all terramechanics math is SI (m, N, Pa/kPa, rad); conversion to UE units happens only at the force/position boundary. All new files are module-root-relative includes (no Public/Private split). Wheel local frame convention: the wheel spins about the block's local **Y**, rolls along local **X**, and the suspension axis is local **–Z**. Steering rotates the wheel frame about local Z.

---

## 1. Data: FVehicleWheelSpec and MeshRelativeTransform

**Decision: inline `USTRUCT(BlueprintType) FVehicleWheelSpec` on `UVehicleBlockDefinitionDataAsset`, gated by a `bIsWheel` EditCondition — not a sub-asset.** Justification: the relationship is strictly 1:1 (one wheel block, one spec), save/load resolves definitions only through `FPrimaryAssetId("VehicleBlock", BlockId)` so a sub-asset would either need a sixth `+PrimaryAssetTypesToScan` line in `Config/DefaultGame.ini` or live as an unmanaged loose asset, and the bootstrap script authors flat props in one `set_props` call — a nested `BlueprintType` struct gets a Python wrapper (`unreal.VehicleWheelSpec`) and stays one call, while a sub-asset needs a second `ensure_data_asset` pass plus cross-asset wiring. `MaxThrust` already set the precedent for module-specific fields on the shared asset; this is the same pattern with a struct so it does not sprawl.

New file: `Source/Exoneer/Vehicles/VehicleWheelSpec.h` (struct only, so `WheelModule.h` and the data asset can both include it cheaply):

```cpp
// Vehicles/VehicleWheelSpec.h
USTRUCT(BlueprintType)
struct FVehicleWheelSpec
{
    GENERATED_BODY()

    // Geometry (m)
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Wheel", meta=(ClampMin="0.05"))
    float RadiusM = 0.35f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Wheel", meta=(ClampMin="0.02"))
    float WidthM = 0.22f;                       // Bekker 'b'

    // Suspension (N/m, N·s/m, m)
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Suspension") float SpringRateNPerM = 42000.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Suspension") float DamperNSecPerM = 3600.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Suspension") float RestLengthM = 0.30f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Suspension") float TravelM = 0.18f;

    // Drivetrain (DC motor curve: T(w) = MaxMotorTorque * (1 - |w|/NoLoadSpeed))
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Drive") bool bDriven = true;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Drive") float MaxMotorTorqueNm = 250.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Drive") float NoLoadSpeedRadS = 30.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Drive") float MaxBrakeTorqueNm = 600.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Drive", meta=(ClampMin="0.1", ClampMax="1"))
    float DrivetrainEfficiency = 0.85f;

    // Steering
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Steer") bool bSteerable = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Steer") float MaxSteerAngleDeg = 35.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Steer") float SteerRateDegPerS = 60.f;

    // Tire (kPa). Ground pressure model: p = InflationPressure + CarcassStiffness
    // (Bekker's flexible-tire approximation; CTIS varies the first term only).
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Tire") float NominalTirePressureKPa = 180.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Tire") float MinTirePressureKPa = 60.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Tire") float MaxTirePressureKPa = 300.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Tire") float CarcassStiffnessKPa = 35.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Tire") float CtisRateKPaPerS = 15.f;   // physical valve rate

    // kg·m². 0 = derive as 0.5 * Def->Mass * RadiusM^2 (solid disc).
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Wheel") float WheelInertiaOverrideKgM2 = 0.f;

    // Bearing/seal drag torque at the hub (N·m), always opposing spin.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Wheel") float BearingDragNm = 1.5f;
};
```

On `Source/Exoneer/Data/VehicleBlockDefinitionDataAsset.h`, after `MaxThrust`:

```cpp
/** True for wheel blocks: excluded from ISMC visuals, gets a UWheelModule + dedicated mesh component. */
UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Wheel")
bool bIsWheel = false;

UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Wheel", meta=(EditCondition="bIsWheel"))
FVehicleWheelSpec WheelSpec;

/** Applied between the block's local frame and the mesh, before the cell-fit scale.
 *  Lets the Z-aligned engine cylinder stand in for a Y-axis wheel (RotX = 90). */
UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Visual")
FTransform MeshRelativeTransform = FTransform::Identity;
```

`MeshRelativeTransform` is honored in both visual paths: composed into the ISMC instance transform in `RebuildDerivedState` (generic win for all blocks) and into the per-wheel `UStaticMeshComponent` relative transform (section 6). For the placeholder: cylinder rotated 90 deg about X, scaled to `(2r, 2r, Width)` in its own frame before the rotation.

One more small def flag that unblocks live building of low wheels (report obstacle: `CanPlaceBlock` world-overlap rejects a wheel ghost intersecting terrain when the hull rests on the ground):

```cpp
/** Skip the world-static overlap rejection when placing this block (wheels sit in terrain by design). */
UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Block")
bool bAllowTerrainOverlapOnPlace = false;   // true for wheels
```

---

## 2. Soil source and gravity

New file pair `Source/Exoneer/Physics/ExoneerSoilPhysicalMaterial.h/.cpp`:

```cpp
UCLASS(BlueprintType)
class EXONEER_API UExoneerSoilPhysicalMaterial : public UPhysicalMaterial
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Soil") FText SoilDisplayName;

    // Bekker pressure-sinkage: p = (Kc/b + Kphi) * z^n, p in kPa
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Soil") float KcKNPerM_Nplus1 = 5.3f;    // k_c  [kN/m^(n+1)]
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Soil") float KphiKNPerM_Nplus2 = 1515.f;// k_phi[kN/m^(n+2)]
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Soil", meta=(ClampMin="0.2")) float SinkageExponentN = 1.1f;

    // Shear strength (Mohr-Coulomb + Janosi-Hanamoto)
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Soil") float CohesionKPa = 1.0f;        // c
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Soil") float FrictionAngleDeg = 31.f;   // phi
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Soil", meta=(ClampMin="0.001")) float ShearModulusKM = 0.025f;  // K, longitudinal
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Soil", meta=(ClampMin="0.001")) float ShearModulusKyM = 0.030f; // K_y, lateral

    FSoilParams ToSoilParams() const;   // plain struct for the math library
};
```

(Defaults above are Wong's published dry-sand values so an unauthored asset is still physical.) `UPhysicalMaterial` lives in PhysicsCore, already a dependency; no Build.cs change.

**Resolution order per wheel ray** (game thread, once per frame):
1. `Params.bReturnPhysicalMaterial = true`; `Cast<UExoneerSoilPhysicalMaterial>(Hit.PhysMaterial.Get())` — authoritative.
2. Fallback: biome default. New field on `UPlanetBiomeDataAsset`: `UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Biome") TObjectPtr<UExoneerSoilPhysicalMaterial> DefaultSoil;` reached through a new `const UExoneerSoilPhysicalMaterial* AVehicleConstruct::GetBiomeDefaultSoil() const` that copies the file-static weak-cache pattern of `GetSunFraction` (`VehicleConstruct.cpp:277-295`). No new primary asset type: the physmat is cooked as a hard reference from the biome asset, so `Config/DefaultGame.ini` is untouched.
3. Final fallback: a code-default `FSoilParams` in `ExoneerTerramechanics.h` (`ExoneerTerramechanics::FirmGroundDefault()`) representing near-rigid ground: very high `k_phi`, `c = 0`, `phi = atan(PhysMaterial ? PhysMaterial->Friction : 0.7)`. This gives one code path for rigid surfaces too — z collapses toward 0, R toward 0, traction becomes Coulomb-limited through the same Janosi expression. No special-case friction curve.

**Gravity: wire `Biome->GravityZ` this pass.** Bekker's `W` comes from the suspension spring, but the spring equilibrium and every mass-derived quantity depend on world gravity, and having a declared-but-dead `GravityZ` while the model claims realism is exactly the kind of silent contradiction the quality bar forbids. The plumbing is six lines in `APlanetEnvironmentManager::BeginPlay` (runs on server and client, deterministic from the asset):

```cpp
if (Biome)
{
    AWorldSettings* WS = GetWorld()->GetWorldSettings();
    WS->bGlobalGravitySet = true;
    WS->GlobalGravityZ = Biome->GravityZ;
}
```

Every consumer — Chaos bodies, `UCharacterMovementComponent`, and the wheel module (`GetWorld()->GetGravityZ()`) — then reads one source. Wheel code never hardcodes 980.

---

## 3. UWheelModule and the new Shutdown hook

`Source/Exoneer/Vehicles/VehicleModule.h` gains the missing destruction hook:

```cpp
/** SERVER. Called exactly once before the construct drops its reference
 *  (block removed, phase regressed, or records moved by split detection).
 *  Base implementation is empty; overrides must be safe to call mid-tick. */
virtual void Shutdown() {}
```

Called at all four removal sites in `VehicleConstruct.cpp`, each in the form `if (UVehicleModule* M = Modules.FindRef(Id)) { M->Shutdown(); } Modules.Remove(Id);`:
1. `RemoveBlock` (`.cpp:456`),
2. `SyncModulesToRecords` stale drop (`.cpp:1039-1045`, before `It.RemoveCurrent()`),
3. split detection scrap path (`.cpp:1608`),
4. split detection record-move path (`.cpp:1634`).

New file pair `Source/Exoneer/Vehicles/WheelModule.h/.cpp`:

```cpp
UCLASS()
class EXONEER_API UWheelModule : public UVehicleModule
{
    GENERATED_BODY()
public:
    virtual void Initialize(AVehicleConstruct* InConstruct, int32 InBlockInstanceId) override;
    virtual void Shutdown() override;
    virtual void TickModule(float DeltaSeconds) override;   // game thread, TG_PrePhysics
    virtual float GetCurrentDraw() const override;

    // Set by AVehicleConstruct::ServerRouteDrive each tick
    float ThrottleCommand = 0.f;        // -1..1
    float BrakeCommand = 0.f;           // 0..1 service brake
    bool  bParkingBrake = false;
    float TargetSteerAngleRad = 0.f;    // Ackermann-resolved by the construct
    float TargetSlipCap = 1.f;          // talent hook (Shear Control lowers this to ~0.2)

    // CTIS (talent hook). Rate-limited by Spec.CtisRateKPaPerS inside TickModule.
    void SetTargetTirePressure(float KPa);
    float GetTirePressureKPa() const { return TirePressureKPa; }

private:
    void SubstepPhysics(float DeltaTime, FBodyInstance* Body);   // bound via FCalculateCustomPhysics

    // --- persistent physical state (server) ---
    float TirePressureKPa = 0.f;        // restored from save or Spec.Nominal
    float TargetTirePressureKPa = 0.f;
    float SteerTrimRad = 0.f;           // restored from save
    float SteerAngleRad = 0.f;          // slewed toward target at SteerRateDegPerS
    float OmegaRadS = 0.f;              // wheel angular velocity, integrated per substep
    float SuspensionCompressionM = 0.f; // last solved, for state readout + visuals
    float WheelInertiaKgM2 = 1.f;       // resolved once in Initialize

    // --- per-frame ground cache, written on game thread BEFORE physics, read in substeps ---
    struct FGroundCache
    {
        bool bHasContact = false;
        FVector PlanePoint = FVector::ZeroVector;   // UU
        FVector PlaneNormal = FVector::UpVector;
        ExoneerTerramechanics::FSoilParams Soil;
        FTransform BlockLocal;                      // block-in-body transform, cached
    } Ground;

    // --- per-substep outputs, read by ledger/replication next game tick ---
    float LastSlipRatio = 0.f;
    float LastSinkageM = 0.f;           // also feeds next substep's contact geometry (one-step lag)
    float LastNormalLoadN = 0.f;
    float LastDriveTorqueNm = 0.f;

    bool bShutDown = false;
};
```

**Lifecycle.** `Initialize`: resolve `WheelInertiaKgM2` (override or `0.5 * Def->Mass * r^2`), consume any pending saved state from the construct (`TakeSavedWheelState`, section 10), set `TirePressureKPa` (saved value or `Spec.NominalTirePressureKPa`), and add this wheel's entry to the replicated side array (section 5). `Shutdown`: set `bShutDown = true` (the substep delegate early-outs on it — a delegate registered earlier in the same frame may still fire after removal), remove the side-array entry and `MarkArrayDirty`. `TickModule` each frame: slew steer angle and tire pressure (rate-limited valve), run the ground raycast, refresh `Ground`, then register `PhysicsRoot->GetBodyInstance()->AddCustomPhysics(FCalculateCustomPhysics::CreateUObject(this, &UWheelModule::SubstepPhysics))` — the delegate list is consumed each physics step, so registration is per frame by design. Finally write the quantized state into the side array under deadbands.

Threading note, stated once and relied on everywhere: `Ground` is written in TG_PrePhysics and read during the solver advance of the same frame; the game thread does not touch it again until the next tick, which is strictly after all substeps complete. Plain floats written in `SubstepPhysics` (`Last*`) are read by the game thread only on the next tick. No locks needed.

---

## 4. Physics integration: per-substep terramechanics against a frame-cached plane

**Raycast (game thread, in `TickModule`).** Dedicated trace channel, because ghost boxes block `ECC_Visibility` and would read as ground:

`Config/DefaultEngine.ini`:
```ini
[/Script/Engine.CollisionProfile]
+DefaultChannelResponses=(Channel=ECC_GameTraceChannel1,DefaultResponse=ECR_Block,bTraceType=True,bStaticObject=False,Name="WheelProbe")
```
plus a named constant `#define ECC_WheelProbe ECC_GameTraceChannel1` in `Vehicles/ExoneerVehicleUnits.h`. `DefaultResponse=ECR_Block` means stock `BlockAll` terrain blocks it with zero content changes, while ghost boxes (explicit ignore-all, then visibility-only block, `.cpp:1344-1348`) stay transparent to it. Base-piece ghosts are transparent to it too, but only because `ABasePiece::RefreshVisualState` sets `ECC_WheelProbe` to `ECR_Ignore` in its Ghost branch: the mesh keeps the `BlockAll` profile, which leaves the probe channel at the project default of Block, so without that one line an unbuilt deck caught a wheel and was asked to carry load it had not been welded to carry. Other constructs' Complete boxes block it – correct, driving onto another vehicle resolves as rigid ground through the firm-ground fallback. A Complete base piece blocks it by design: that hit is how a deck learns which wheels are standing on it (`UWheelModule::GroundPiece` → `ABasePiece::ReportLiveLoad`).

```cpp
FCollisionQueryParams Params(SCENE_QUERY_STAT(ExoneerWheelProbe), false, Construct.Get());
Params.bReturnPhysicalMaterial = true;
const FTransform BlockWorld = Construct->GetBlockWorldTransform(*FindRecord());
const FVector Axis = -BlockWorld.GetUnitAxis(EAxis::Z);
const float RayLen = (Spec.RestLengthM + Spec.TravelM + Spec.RadiusM) * MToUU + 5.f;
GetWorld()->LineTraceSingleByChannel(Hit, BlockWorld.GetLocation(), BlockWorld.GetLocation() + Axis * RayLen, ECC_WheelProbe, Params);
```

Miss → `Ground.bHasContact = false`. Hit → cache plane point/normal and resolve soil per section 2. Scene queries never run on the physics thread; the substep callback re-solves suspension compression **against the cached plane analytically**, so it tracks body motion within the frame (the plane is stale by at most one frame — acceptable on continuous terrain, and the substep spring damping absorbs it).

**`SubstepPhysics(float DeltaTime, FBodyInstance* Body)` — the full chain per substep:**

1. Body state via the `_AssumesLocked` interface (the callback holds the scene lock): body transform, hub world position `HubW = BodyTM.TransformPosition(Ground.BlockLocal.GetLocation())`, point velocity at hub `V_hub`, wheel basis vectors (block local X/Y/Z rotated by body + steer rotation about local Z).
2. **Airborne branch** (`!Ground.bHasContact`): no body forces (the solver applies gravity). Wheel spin only: `I*dw = (T_motor(w, u) - BearingDrag*sgn(w)) * dt` — a free wheel spins up along the motor curve and coasts down on bearing drag. Suspension state relaxes to rest length for visuals. Return.
3. **Suspension.** Distance from hub to cached plane along the suspension axis, minus wheel radius, minus last substep's sinkage (`LastSinkageM` — the z/W circular dependency is broken with a one-substep lag, which converges in 2-3 substeps at 60-240 Hz): gives current length; `x = clamp(RestLength - length, 0, Travel)`; `xdot` from `V_hub` projected on the axis. Spring-damper: `Fs = SpringRate*x + Damper*xdot`, clamped `>= 0` (a suspension cannot pull). Normal load `W = Fs` (N).
4. **Ground pressure and sinkage** (SI): `p = TirePressureKPa + Spec.CarcassStiffnessKPa` (Bekker's flexible-tire approximation — this is precisely the variable CTIS moves); sinkage
   `z = ExoneerTerramechanics::BekkerSinkage(p, b, kc, kphi, n)` implementing `z = (p / (k_c/b + k_phi))^(1/n)` exactly as GAME-SCOPE 4.1.
5. **Contact patch** from load equilibrium: `A = W / (p * 1000)` (m²), `l = clamp(A / b, 0.05*r, 1.5*r)`.
6. **Slip ratio** (Wong's definitions, unified): longitudinal ground speed `v_x = dot(V_hub, WheelForward)` in m/s; driving `s = (w*r - v_x)/max(|w*r|, eps)`, braking `s = (w*r - v_x)/max(|v_x|, eps)`; eps = 0.1 m/s. `TargetSlipCap` clamps commanded torque when `|s|` exceeds it (Shear Control talent hook — a torque cap on a physical variable, not a percentage buff).
7. **Tractive effort** — `F = (A*c + W*tan(phi)) * (1 - (K/(s*l)) * (1 - e^(-s*l/K)))`, exact GAME-SCOPE form, with the analytic `s -> 0` limit (`-> s*l/(2K)` factor) so it never divides by zero. Sign follows slip sign.
8. **Compaction resistance** — `R = b * (k_c/b + k_phi) * z^(n+1) / (n+1)` (kN → N), applied opposing the longitudinal hub velocity direction only while `|v_x| > eps`.
9. **Lateral force**: slip angle `alpha = atan2(dot(V_hub, WheelRight), |v_x|)`; lateral shear displacement `j_y = tan(alpha)*l`; `F_y = (A*c + W*tan(phi)) * (1 - e^(-|j_y|/K_y))`, opposing lateral velocity. Combined-slip budget: scale `(F, F_y)` so `sqrt(F^2 + F_y^2) <= A*c + W*tan(phi)` (friction ellipse on the same shear-strength ceiling — still the published limit, no invented curve).
10. **Wheel spin integration**: `T_motor = ThrottleCommand * MaxMotorTorque * (1 - |w|/NoLoadSpeed) * PowerSupplyFraction` (linear DC motor curve; brownouts scale torque physically). `I*dw = (T_motor - T_brake*sgn(w) - F*r - BearingDrag*sgn(w)) * dt`, where `T_brake = BrakeCommand*MaxBrakeTorque + (bParkingBrake ? MaxBrakeTorque : 0)`.
11. **Apply to the body** at the contact point (`HubW - WheelUp*(r - z)` in UU): `W` along the plane normal, `F` along wheel-forward-projected-on-plane, `-R` along the hub velocity direction, `F_y` along wheel-right-projected-on-plane. All via `Body->AddForceAtPosition(ForceUE, PosUU, /*bAllowSubstepping=*/false)` so each substep gets its own evaluation. Conversion at this line only: `ForceUE = ForceN * NewtonsToUEForce`.
12. Store `Last*` outputs.

Also fix the dead config key while touching the file: `PhysicSubstepDeltaTime=0.0166` → `MaxSubstepDeltaTime=0.008333` and `MaxSubsteps=6` — the slip feedback loop is the one consumer that actually needs the tighter substep the old key silently failed to provide.

---

## 5. Replication: FVehicleWheelStateList side array

New file `Source/Exoneer/Vehicles/VehicleWheelState.h`:

```cpp
USTRUCT()
struct FVehicleWheelStateItem : public FFastArraySerializerItem
{
    GENERATED_BODY()
    UPROPERTY() int32 BlockInstanceId = INDEX_NONE;
    UPROPERTY() int16 SteerAngleQ = 0;      // rad * 1000
    UPROPERTY() int16 OmegaQ = 0;           // rad/s * 64
    UPROPERTY() uint8 SlipQ = 0;            // |s| 0..1 -> 0..255
    UPROPERTY() uint8 SinkageQ = 0;         // z in cm, 0..255
    UPROPERTY() uint8 CompressionQ = 0;     // x / Travel -> 0..255 (suspension visual)
    UPROPERTY() uint8 TirePressureQ = 0;    // kPa / 2
    // Callbacks deliberately EMPTY - never MarkVisualsDirty. Visual consumption is polled in Tick.
    void PostReplicatedAdd(const struct FVehicleWheelStateList&) {}
    void PostReplicatedChange(const struct FVehicleWheelStateList&) {}
    void PreReplicatedRemove(const struct FVehicleWheelStateList&) {}
};

USTRUCT()
struct FVehicleWheelStateList : public FFastArraySerializer
{
    GENERATED_BODY()
    UPROPERTY() TArray<FVehicleWheelStateItem> Items;
    bool NetDeltaSerialize(FNetDeltaSerializeInfo& P)
    { return FastArrayDeltaSerialize<FVehicleWheelStateItem, FVehicleWheelStateList>(Items, P, *this); }
};
template<> struct TStructOpsTypeTraits<FVehicleWheelStateList>
    : TStructOpsTypeTraitsBase2<FVehicleWheelStateList> { enum { WithNetDeltaSerializer = true }; };
```

On `AVehicleConstruct`: `UPROPERTY(Replicated) FVehicleWheelStateList WheelStates;` plus lookup helpers. The server (each `UWheelModule::TickModule`) writes with deadbands — steer 0.01 rad, omega 0.25 rad/s, slip/sinkage 1 quantum — and slip/sinkage/pressure additionally rate-limited to ~5 Hz (dashboard data, not animation data). `MarkItemDirty` per changed item only.

**Split detection**: at the record-move site (`.cpp:1621-1637`) move matching items into the new construct's `WheelStates` (reset `ReplicationID/Key` like the records) and remove them from the parent; at the scrap site (`.cpp:1602-1609`) remove them. `UWheelModule::Shutdown` also removes its own item, which covers sites 1 and 2; the split sites drop modules of moved blocks via those same hooks, so the transfer of *items* is the only extra code.

**Aggregate accessor** (both sides, HUD-safe), in `VehicleConstruct.h`:

```cpp
USTRUCT(BlueprintType)
struct FVehicleDrivetrainSummary
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) float WorstSlipRatio = 0.f;
    UPROPERTY(BlueprintReadOnly) float MaxSinkageM = 0.f;
    UPROPERTY(BlueprintReadOnly) float MinTirePressureKPa = 0.f;
    UPROPERTY(BlueprintReadOnly) float SpeedMS = 0.f;           // from replicated velocity
    UPROPERTY(BlueprintReadOnly) int32 WheelCount = 0;
    UPROPERTY(BlueprintReadOnly) int32 WheelsInContact = 0;     // SinkageQ/CompressionQ > 0 heuristic on clients
    UPROPERTY(BlueprintReadOnly) uint8 ControlMode = 0;         // EPilotControlMode
};

UFUNCTION(BlueprintPure, Category="Vehicle")
FVehicleDrivetrainSummary GetDrivetrainSummary() const;   // folds WheelStates + GetVelocity()
```

The interim visor HUD (`ExoneerHUD::DrawPilotPanel`, new) consumes only this accessor — speed, power fraction, worst slip with the 0.15–0.25 optimum window marked, max sinkage, tire pressure — using the existing `DrawReadout` primitive and the Tick-side `FInterpTo` smoothing pattern, so the future diegetic dashboard swaps the renderer, not the data source.

---

## 6. Wheel visuals: per-wheel UStaticMeshComponent

In `RebuildDerivedState` (runs on **both** sides):
- Records with `Def->bIsWheel && Phase == Complete` are skipped when filling `CompleteInstances` (ghost/under-construction wheels still render through the ghost ISMC — they do not animate).
- New member `UPROPERTY() TMap<int32, TObjectPtr<UStaticMeshComponent>> WheelVisuals;` — create-if-missing per Complete wheel record: `NewObject<UStaticMeshComponent>`, mesh from `Def->Mesh`, `SetCollisionEnabled(NoCollision)`, attach to `PhysicsRoot` KeepRelative, relative transform = block local transform composed with `MeshRelativeTransform` and the radius/width scale. Cleanup by live-id sweep exactly like `BlockBodies` (`.cpp:1357-1367`): any keyed component whose id is no longer a Complete wheel is destroyed and removed.
- Full rebuilds recreating the component are harmless because the animation pose is re-applied every frame (next point).

New both-sides block in `AVehicleConstruct::Tick`, `UpdateWheelVisuals(float Dt)`:
- steer + suspension + sinkage from `WheelStates` (server could read modules directly, but reading the same replicated array on both sides keeps one code path; the server wrote it this frame);
- spin: each visual keeps a locally integrated angle in a transient `TMap<int32, float> WheelSpinAngle` — `Angle += DequantizedOmega * Dt` (client never needs spin *position* replicated; rate is enough and drift is invisible);
- pose: `Relative = BlockLocal * FTransform(SteerRotZ) * FTransform(SpinRotY) * MeshRelativeTransform`, then translate along block local –Z by `RestLength - Compression + SinkageM` (suspension droop plus visual burial by z — deformable contact-patch mesh comes later, the offset is the honest placeholder).

---

## 7. Pilot input redesign

New file `Source/Exoneer/Vehicles/PilotInput.h`:

```cpp
UENUM(BlueprintType)
enum class EPilotControlMode : uint8 { Ground, Flight };

USTRUCT(BlueprintType)
struct FPilotInput
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadWrite) FVector Move = FVector::ZeroVector;    // thruster intent (Flight)
    UPROPERTY(BlueprintReadWrite) FVector Rotate = FVector::ZeroVector;  // gyro intent (Flight)
    UPROPERTY(BlueprintReadWrite) float Throttle = 0.f;                  // -1..1 (Ground)
    UPROPERTY(BlueprintReadWrite) float Steer = 0.f;                     // -1..1 (Ground)
    UPROPERTY(BlueprintReadWrite) float Brake = 0.f;                     // 0..1
    /** Four 2-bit rolling counters: ParkingBrake, CtisUp, CtisDown, ControlModeToggle. */
    UPROPERTY(BlueprintReadWrite) uint8 LatchedFlags = 0;

    bool NetSerialize(FArchive& Ar, UPackageMap*, bool& bOutSuccess);    // int8 per axis component,
};                                                                        // int8 throttle/steer, uint8 brake+flags: 10 bytes
template<> struct TStructOpsTypeTraits<FPilotInput>
    : TStructOpsTypeTraitsBase2<FPilotInput> { enum { WithNetSerializer = true }; };
```

**Latching semantics.** Discrete actions are 2-bit rolling counters, not momentary bits: the client increments its local counter on key press and the struct always carries current counter values, so a press between 20 Hz sends is never lost and a dropped unreliable packet is healed by the next send (the counter is still ahead). The server edge-detects `counter != last_seen_counter` and executes once per increment. Two presses inside one lost-packet window survive (counter advances by 2); four lost sends in a row is the theoretical loss window and acceptable for cabin toggles.

**Three call sites changed in lockstep:**
1. `PlayerSurvivalCharacter.h:164-165` → `UFUNCTION(Server, Unreliable, WithValidation) void Server_SendPilotInput(AVehicleConstruct* Construct, FPilotInput Input);` (validation still NaN-only). Client accumulation becomes an `FPilotInput PendingPilotInput` member; axes are written by `Input_Move/Input_Look/Input_Jump` according to the control mode; counters by the new bound actions.
2. `AVehicleConstruct::SetPilotInput(const FPilotInput& Input)` (`VehicleConstruct.h:192`, `.cpp:815`) — stores `LastPilotInput` (axes clamped) and `LastPilotInputTime = GetWorld()->GetTimeSeconds()`, and processes the four counters.
3. `IPilotable::ApplyPilotInput(const FPilotInput& Input)` (`Interfaces/Pilotable.h`) — signature swap of the BlueprintNativeEvent; the thin forwarder implementation follows.

**Decay replaced by hold-last-with-timeout.** Delete `PendingMove/PendingRotate *= (1 - dt*8)` (`.cpp:1008-1010`). The server holds `LastPilotInput` as-is; if `Now - LastPilotInputTime > 0.5f` (`UPROPERTY PilotInputTimeoutS = 0.5f`, ten missed packets at 20 Hz — a real link-loss, not jitter), axes zero and the parking brake engages (a rover with a dead command link should stop, physically, through its brakes — not through fake drag). No more 40 percent sag between packets; `PilotInputSendHz` stays 20.

**Control mode.** `UPROPERTY(Replicated) EPilotControlMode ControlMode;` on the construct. Server default whenever wheel-module presence changes in `SyncModulesToRecords`: any Complete driven wheel → `Ground`, else `Flight`; a manual toggle (counter action, free key **V** → `IA_ToggleControlMode`) overrides until the pilot exits. Client behavior split: in `Ground` mode `Input_Look` falls through to the normal camera path (the pilot can look around) and `Input_Move` writes `Throttle = Axis.Y`, `Steer = Axis.X`; in `Flight` mode current behavior is preserved verbatim. New input actions (bootstrap: names must exactly equal the C++ UPROPERTY names for the blind CDO wiring loop): `IA_ToggleControlMode` (V), `IA_Handbrake` (LeftControl), `IA_TirePressure` (Axis1D on MouseWheelAxis → CtisUp/CtisDown counters), plus `IA_Brake` (Z, held → `Brake = 1`).

**Server-side routing.** `ServerRouteThrust` gains a sibling `ServerRouteDrive(float Dt)`: commanded curvature `kappa = Steer * MaxKappa` (MaxKappa from the steerable wheels' max angle and the geometric wheelbase); per steerable wheel proper Ackermann from geometry — steer angle `= atan2(x_i * kappa, 1 - y_i * kappa)` about the instantaneous center, where `(x_i, y_i)` are the wheel's offsets from the centroid of non-steerable wheels in the cockpit frame; per driven wheel `ThrottleCommand = Throttle`, `BrakeCommand = Brake`, `bParkingBrake` from the latched state. In Ground mode `ServerRouteThrust`'s thruster/gyro path is skipped (thrusters stay available in Flight).

---

## 8. Damping and gyro

`AVehicleConstruct` tunables (replacing the two hardcoded lines at `.cpp:140-141`):

```cpp
UPROPERTY(EditAnywhere, Category="Vehicle|Physics") float LinearDampingNoWheels = 0.4f;
UPROPERTY(EditAnywhere, Category="Vehicle|Physics") float AngularDampingNoWheels = 2.0f;
UPROPERTY(EditAnywhere, Category="Vehicle|Physics") float LinearDampingWheeled = 0.01f;
UPROPERTY(EditAnywhere, Category="Vehicle|Physics") float AngularDampingWheeled = 0.05f;
```

Applied in `RebuildDerivedState` whenever Complete-wheel presence flips: with wheels, damping drops to the near-zero floor (numerical hygiene only) so Bekker `R`, the Janosi traction limit, and bearing drag are the *only* motion resistance — otherwise the tuning is meaningless. Wheel-less constructs keep the legacy values.

Gyro: `RotationTorquePerKg = 800` currently grants free attitude authority. New `UPROPERTY(EditAnywhere) float GroundModeGyroFraction = 0.f;` — in `Ground` mode the gyro torque in `ServerRouteThrust` is multiplied by it (default 0: a rover has no reaction wheels; mid-air it is ballistic, which is the realism scope). Flight mode is unchanged. If later talents add an attitude-assist flywheel, it raises this fraction through a physical variable, not a buff.

---

## 9. Power ledger integration

`UWheelModule::GetCurrentDraw()`:

```cpp
const float Pmech = FMath::Abs(LastDriveTorqueNm * OmegaRadS);            // W, mechanical
const float Pelec = Pmech / Spec.DrivetrainEfficiency
                  + FMath::Abs(ThrottleCommand) * 0.1f * FMath::Abs(Def->PowerDelta); // controller + stall floor
return FMath::Min(Pelec, FMath::Abs(Def->PowerDelta));                    // rated ceiling
```

`Def->PowerDelta` (negative) is the motor's rated electrical wattage. The ledger runs before `ServerRouteDrive` in `Tick`, so demand is one frame stale — same accepted behavior as thrusters, documented in a comment at the ledger call site so it stays a decision, not a surprise. Supply-side scaling is already physical: `PowerSupplyFraction` multiplies available motor torque inside `SubstepPhysics` (step 10), so a brownout is a weaker motor, and a stalled climb draws the stall floor without exceeding rating.

---

## 10. Save/load

`Save/ExoneerSaveGame.h`, `FSavedVehicleBlock` additions (tagged-property serialization makes old saves load these at defaults — default-safe by construction):

```cpp
UPROPERTY() float TirePressureKPa = 0.f;   // 0 = "use Spec.Nominal" on load
UPROPERTY() float SteerTrimDeg = 0.f;
```

Save side (`SaveGameSubsystem.cpp:139-162`): for wheel records, pull the values from the live module via a new server accessor `bool AVehicleConstruct::GetWheelPersistentState(int32 Id, float& OutPressure, float& OutTrim) const`.

Load side, two fixes plus restore plumbing:
1. **Overlap bypass — chosen over transform lifting.** New server flag `bool bSuppressWorldOverlapCheck = false` on `AVehicleConstruct`, checked inside `CanPlaceBlock` around the `ECC_WorldStatic` overlap (`.cpp:323-351`); `ApplyVehicles` sets it true on the freshly spawned construct for the whole replay loop and clears it after `RestoreBlockRecord`. Justification: lifting the spawn transform changes the parked pose every save/load cycle (accumulating drift) and still cannot guarantee clearance on concave terrain; bypass restores byte-exact geometry, and the suspension settles the body on the first simulated frame anyway. The same code path honors the per-definition `bAllowTerrainOverlapOnPlace` (section 1) for live building.
2. **FoundConstruct guard**: before re-basing, `ApplyVehicles` finds the first saved block whose resolved `Def->ModuleClass == nullptr` and swaps it to index 0 (fallback: keep `Blocks[0]` if every block is a module), matching the build tool's own founding rule (`BuildToolComponent.cpp:788-792`).
3. **Restore**: after `RestoreBlockRecord`, call new `void AVehicleConstruct::RestoreWheelState(int32 BlockInstanceId, float TirePressureKPa, float SteerTrimDeg)` which stores into a server-only `TMap<int32, FWheelSavedState> PendingWheelRestore`; `UWheelModule::Initialize` consumes-and-removes its entry (`TakeSavedWheelState`). This survives the fact that modules are created later by `SyncModulesToRecords`, not at restore time.

---

## 11. Pure math library and automation tests

New files `Source/Exoneer/Vehicles/ExoneerTerramechanics.h/.cpp` — namespace of static pure functions, SI in/out, no UObject, no engine state, fully unit-testable:

```cpp
namespace ExoneerTerramechanics
{
    struct FSoilParams { float KcKN; float KphiKN; float N; float CohesionKPa; float FrictionAngleRad; float KM; float KyM; };
    FSoilParams FirmGroundDefault(float CoulombMu = 0.7f);

    /** z = (p / (k_c/b + k_phi))^(1/n).  p in kPa, b in m, returns m. */
    float BekkerSinkage(float GroundPressureKPa, float WidthM, const FSoilParams& Soil);

    /** F = (A*c + W*tan(phi)) * (1 - (K/(s*l)) * (1 - exp(-s*l/K))). Analytic s->0 limit. Returns N, signed by slip. */
    float TractiveEffort(float ContactAreaM2, float NormalLoadN, float SlipRatio,
                         float PatchLengthM, const FSoilParams& Soil);

    /** R = b * (k_c/b + k_phi) * z^(n+1) / (n+1). Returns N. */
    float CompactionResistance(float WidthM, float SinkageM, const FSoilParams& Soil);

    float SlipRatio(float OmegaRadS, float RadiusM, float LongitudinalSpeedMS, float EpsMS = 0.1f);
    float LateralShearForce(float ContactAreaM2, float NormalLoadN, float SlipAngleRad,
                            float PatchLengthM, const FSoilParams& Soil);          // Janosi with K_y
    float ContactPatchLength(float NormalLoadN, float GroundPressureKPa, float WidthM, float RadiusM);
    float MotorTorque(float MaxTorqueNm, float NoLoadSpeedRadS, float OmegaRadS, float Command01);
}
```

Shared constants in new `Source/Exoneer/Vehicles/ExoneerVehicleUnits.h` — `NewtonsToUEForce = 100.f` promoted out of `VehicleModule.cpp:62` (thruster code switches to it), plus `MToUU = 100.f`, `ECC_WheelProbe`.

Tests: new `Source/Exoneer/Tests/ExoneerTerramechanicsTests.cpp`, wrapped in `#if WITH_DEV_AUTOMATION_TESTS`, using `IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerTerramechanicsSinkageTest, "Exoneer.Terramechanics.Sinkage", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)` and siblings for traction, resistance, slip, and limits. Assertions against published anchors and analytic properties: hand-computed z for Wong's dry-sand constants at known p/b; `TractiveEffort` → 0 as s → 0 and → `A*c + W*tan(phi)` as s → large; monotonic z in p; `R ∝ z^(n+1)`; the s → 0 series limit matches the full expression at s = 1e-3 within 1e-4; slip-ratio sign conventions for drive vs brake. Headless run for the watcher loop:

```
& "<UE_5.8>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "c:\Users\Mark\Documents\GitHub\Exoneer\Exoneer.uproject" ^
  -ExecCmds="Automation RunTests Exoneer.Terramechanics; Quit" -TestExit="Automation Test Queue Empty" ^
  -unattended -nullrhi -nosplash -nop4 -log
```

---

## 12. File-by-file change list, implementation order

| # | File | New/Mod | Size | Contents |
|---|------|---------|------|----------|
| 1 | `Source/Exoneer/Vehicles/ExoneerVehicleUnits.h` | new | ~30 | `NewtonsToUEForce`, `MToUU`, `ECC_WheelProbe` |
| 2 | `Source/Exoneer/Vehicles/ExoneerTerramechanics.h/.cpp` | new | ~90/~180 | FSoilParams + pure functions (section 11) |
| 3 | `Source/Exoneer/Tests/ExoneerTerramechanicsTests.cpp` | new | ~220 | automation tests; proves the math before any physics wiring |
| 4 | `Config/DefaultEngine.ini` | mod | ~5 | WheelProbe channel; fix `PhysicSubstepDeltaTime` → `MaxSubstepDeltaTime=0.008333`, `MaxSubsteps=6` |
| 5 | `Source/Exoneer/Physics/ExoneerSoilPhysicalMaterial.h/.cpp` | new | ~70/~25 | soil physmat class |
| 6 | `Source/Exoneer/Data/PlanetBiomeDataAsset.h` | mod | ~5 | `DefaultSoil` |
| 7 | `Source/Exoneer/World/PlanetEnvironmentManager.h/.cpp` | mod | ~15 | gravity wiring in BeginPlay; `GetDefaultSoil()` |
| 8 | `Source/Exoneer/Vehicles/VehicleWheelSpec.h` | new | ~90 | FVehicleWheelSpec |
| 9 | `Source/Exoneer/Data/VehicleBlockDefinitionDataAsset.h` | mod | ~15 | `bIsWheel`, `WheelSpec`, `MeshRelativeTransform`, `bAllowTerrainOverlapOnPlace` |
| 10 | `Source/Exoneer/Vehicles/VehicleModule.h/.cpp` | mod | ~10 | `virtual void Shutdown()`; thruster switches to shared `NewtonsToUEForce` |
| 11 | `Source/Exoneer/Vehicles/PilotInput.h` | new | ~90 | FPilotInput + NetSerialize + EPilotControlMode |
| 12 | `Source/Exoneer/Interfaces/Pilotable.h` | mod | ~10 | `ApplyPilotInput(const FPilotInput&)` |
| 13 | `Source/Exoneer/Vehicles/VehicleWheelState.h` | new | ~110 | side fast-array (section 5) |
| 14 | `Source/Exoneer/Vehicles/WheelModule.h/.cpp` | new | ~120/~380 | UWheelModule (sections 3–4, 9) |
| 15 | `Source/Exoneer/Vehicles/VehicleConstruct.h/.cpp` | mod | ~400 | Shutdown at 4 sites; WheelStates + replication + split transfer/purge; WheelVisuals + ISMC exclusion + MeshRelativeTransform; damping tunables; SetPilotInput(FPilotInput) + hold-last timeout; ControlMode + gyro gating; ServerRouteDrive + Ackermann; GetDrivetrainSummary; overlap-bypass flag; RestoreWheelState/GetWheelPersistentState; CTIS/handbrake counter handling |
| 16 | `Source/Exoneer/Player/PlayerSurvivalCharacter.h/.cpp` | mod | ~120 | FPilotInput accumulation, counters, mode-split Input_Move/Look, new IA UPROPERTYs (`IA_ToggleControlMode`, `IA_Handbrake`, `IA_Brake`, `IA_TirePressure`) + bindings |
| 17 | `Source/Exoneer/Save/ExoneerSaveGame.h` | mod | ~6 | two FSavedVehicleBlock fields |
| 18 | `Source/Exoneer/Save/SaveGameSubsystem.cpp` | mod | ~40 | save wheel fields; structural-first founding; overlap bypass around replay; RestoreWheelState calls |
| 19 | `Source/Exoneer/Player/ExoneerHUD.h/.cpp` | mod | ~60 | `DrawPilotPanel` off `GetDrivetrainSummary` |
| 20 | `scripts/BootstrapPrototype.py` | mod | ~120 | `ensure_block(extra=, size_in_cells=)` seam; wheel block (`CYLINDER`, `is_wheel`, wheel spec via `unreal.VehicleWheelSpec`, `mesh_relative_transform`); items/recipes/quick_bar/starter items; `PM_RegolithSoft` via `PhysicalMaterialFactoryNew` + `set_phys_material_override` on a soft-soil test slab; unconditional ensure-actor-by-label map pass (soil patch + ramp); new IA specs + bindings (V, LeftControl, Z, MouseWheelAxis with `AXIS1D`) |

Compile-risk notes for the watcher loop: C4458-as-error means no locals named `Radius`, `Def`, `Health`, `Orientation` etc. inside AVehicleConstruct/UWheelModule methods that shadow members — prefix substep locals (`WheelRadiusM`, `RecordDef`); never name locals or params `Tags` (shadows `AActor::Tags`) or `Instigator` (the existing code already dodges this with `DamageInstigator`); no `UFUNCTION` named `SetActive`/`IsActive` anywhere (the CTIS setter is `SetTargetTirePressure`, plain virtual, not a UFUNCTION); `FPilotInput`/`FVehicleWheelStateList` need their `TStructOpsTypeTraits` specializations in the same header or UHT-visible scope; `VehicleBlockDefinitionDataAsset.h` must include `VehicleWheelSpec.h` (value member, complete type required); forward-declare `UExoneerSoilPhysicalMaterial` in headers and include `PhysicalMaterials/PhysicalMaterial.h` only in the .cpp/its own header.

---

## Resolution of the nine exploration blockers

1. **No module Shutdown hook** → `UVehicleModule::Shutdown()` added and called at all four removal sites: `RemoveBlock` (.cpp:456), `SyncModulesToRecords` stale drop (.cpp:1039-1045), split scrap (.cpp:1608), split move (.cpp:1634). Section 3.
2. **ISMC cannot animate wheels** → Complete wheel blocks excluded from ISMC layers; per-wheel `UStaticMeshComponent` in `TMap<int32, TObjectPtr<UStaticMeshComponent>> WheelVisuals`, created in `RebuildDerivedState` on both sides, cleaned by the same live-id sweep as `BlockBodies`. Section 6.
3. **Continuous state must stay out of FVehicleBlockRecord** → `FVehicleWheelStateList` side fast-array keyed by BlockInstanceId with deliberately empty replication callbacks (never `MarkVisualsDirty`), deadbanded server writes, transferred/purged at both split sites. Section 5.
4. **Forces once per frame vs substepping** → `FBodyInstance::AddCustomPhysics` registered per wheel per frame; the delegate does suspension + Bekker/Janosi + wheel-spin integration per substep against a frame-cached ground plane; game-thread ray only (scene queries unsafe on the physics thread); airborne = no forces, motor-curve spin only. Substep config key fixed. Section 4.
5. **Damping contaminates the model** → four UPROPERTY tunables; near-zero linear/angular damping applied in `RebuildDerivedState` when any Complete wheel exists, legacy values otherwise. Section 8.
6. **Save/load silently drops terrain-intersecting wheels** → `bSuppressWorldOverlapCheck` restore-scope bypass in `CanPlaceBlock` (chosen over transform lifting: exact geometry, no per-cycle drift); `bAllowTerrainOverlapOnPlace` for live wheel placement; `FoundConstruct` guarded to a structural first record. Section 10.
7. **Pilot input decays 40 percent between packets** → `FPilotInput` struct (quantized NetSerialize) through all three call sites; server holds last input, 0.5 s timeout zeroes axes and sets the parking brake; discrete presses carried as 2-bit rolling counters so between-send presses latch losslessly. Section 7.
8. **`NewtonsToUEForce` file-local** → promoted to `Vehicles/ExoneerVehicleUnits.h`, consumed by thruster and wheel code; all terramechanics stays SI with conversion at the single force-application line. Sections 4, 11.
9. **Gyro gives free mid-air attitude authority** → `GroundModeGyroFraction = 0` multiplies gyro torque in Ground mode (wheels present ⇒ Ground default, V toggles); a rover in the air is ballistic; Flight-mode thruster craft unchanged. Section 8.

Also closed from the terrain report: ghost blocks blocking `ECC_Visibility` (dedicated `WheelProbe` channel with `AddIgnoredActor(construct)`), no soil source (`UExoneerSoilPhysicalMaterial` + biome default + firm-ground code fallback), and the dead `GravityZ` (wired into `AWorldSettings::GlobalGravityZ` this pass — one gravity source).

### Critical Files for Implementation
- c:/Users/Mark/Documents/GitHub/Exoneer/Source/Exoneer/Vehicles/VehicleConstruct.cpp
- c:/Users/Mark/Documents/GitHub/Exoneer/Source/Exoneer/Vehicles/WheelModule.cpp (new)
- c:/Users/Mark/Documents/GitHub/Exoneer/Source/Exoneer/Vehicles/ExoneerTerramechanics.cpp (new)
- c:/Users/Mark/Documents/GitHub/Exoneer/Source/Exoneer/Player/PlayerSurvivalCharacter.cpp
- c:/Users/Mark/Documents/GitHub/Exoneer/Source/Exoneer/Save/SaveGameSubsystem.cpp