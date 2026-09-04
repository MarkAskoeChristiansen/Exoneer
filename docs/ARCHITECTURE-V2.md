# Exoneer construction layer v2 – architecture specification

Status: construction layer **v2 implemented**. Sections 16–18 specify the
**v3 seams** (condition, optional projects, persistence) that sit on this
layer; they are not implemented yet.

Serves the permanent design boundary in [GAME-SCOPE.md](GAME-SCOPE.md)
(including §10 causal maintenance) and the product north star in
[VISION.md](VISION.md) (sandbox first, Road to Orbit optional).

Where this document and GAME-SCOPE.md disagree on *behavior*, GAME-SCOPE
wins. Where a feature disagrees with *what the game is*, VISION wins. The
gap map is GAME-SCOPE section 9.

Scope of v2: Building, Components, Machines, Interfaces, Vehicles, World,
Resources, Save. Not in v2: EOS, condition, projects.

## 1. Design goals

1. **Base building (Icarus-inspired, original implementation):** socket-snapped architectural
   pieces (foundation, wall, floor, ramp, roof, beam, door frame) with material tiers,
   structural support rules, and weather damage.
2. **Vehicle building (Space Engineers 2-inspired, original implementation):** volumetric
   blocks on a unified fine grid (25 cm cells) forming one physics-simulated rigid body per
   construct, with mass, center of mass, thrusters, cockpit, and split detection.
3. **Construction flow everywhere:** ghost placement first, then invest components
   ("welding") through construction stages until complete. Deconstruction refunds.
4. **Fully replicated:** 1-4 player listen-server co-op. Server authoritative for all
   state mutations; clients predict nothing in v2 (correctness first).
5. **Data driven:** every piece, block, item, recipe, and biome is a `UPrimaryDataAsset`.
6. **Low coupling:** UInterfaces + Gameplay Tags for interaction, construction, damage,
   and mount compatibility. No direct cross-module class dependencies except through
   interfaces and data assets.

## 2. Module map

```
Source/Exoneer/
  ExoneerGameplayTags.{h,cpp}     NEW  native gameplay tags (mount types, interaction)
  ExoneerTypes.h                  NEW  shared enums/structs (EStructureTier, EMachineState, ...)
  Interfaces/
    Interactable.h                REDO server OnInteract + client OnInteractLocal + tags
    Constructible.h               NEW  ghost/invest/complete lifecycle (replaces Buildable.h)
    Damageable.h                  KEEP
    InventoryOwner.h              KEEP
    PowerConsumer.h/PowerProducer.h KEEP
    OxygenProvider.h              KEEP
    Pilotable.h                   REDO pilot into AVehicleConstruct via cockpit block id
    Buildable.h                   DELETE
  Data/
    ItemDefinitionDataAsset.h     KEEP
    PieceDefinitionDataAsset.h    NEW  base pieces: archetype, tier, sockets, mount rules
    VehicleBlockDefinitionDataAsset.h NEW vehicle blocks: cell size, mass, module class
    RecipeDefinitionDataAsset.h   KEEP
    PlanetBiomeDataAsset.h        KEEP + storm damage fields
    BlockDefinitionDataAsset.h    DELETE
  Components/
    InventoryComponent.{h,cpp}    REDO replicated FFastArraySerializer storage
    CargoComponent.h              KEEP (subclass, volume-based)
    InteractionComponent.{h,cpp}  REDO local focus trace + Server_TryInteract validation
    ConstructionComponent.{h,cpp} NEW  per-piece construction state machine (base pieces)
    BuildToolComponent.{h,cpp}    REDO two placement modes (base sockets / vehicle grid) + weld
    MiningToolComponent.{h,cpp}   REDO client trace -> Server_MineTarget validation
    PowerComponent.{h,cpp}        KEEP + replication of SupplyFraction/StoredEnergy
    PowerNetworkComponent.{h,cpp} REDO replicated snapshot, hosted by ABaseStructure
    ConveyorComponent.{h,cpp}     KEEP semantics + authority guards
    CraftingComponent.{h,cpp}     REDO authority guards + replicated queue summary + state enum
    HealthComponent / OxygenComponent / SurvivalStatsComponent  KEEP (not in redo scope)
  Building/
    BaseStructure.{h,cpp}         NEW  piece registry, socket graph, support solver, power host
    BasePiece.{h,cpp}             NEW  one placed architectural piece
    BlockGridActor / BuildableBlock  DELETE
  Machines/
    MachinePiece.{h,cpp}          NEW  ABasePiece + power/inventory/conveyor + EMachineState
    RefineryPiece / FabricatorPiece / OxygenGeneratorPiece
    BatteryPiece / SolarPanelPiece  RENAMED+REDO from *Block
    CockpitBlock / ThrusterBlock  DELETE (become vehicle modules)
  Vehicles/
    VehicleConstruct.{h,cpp}      NEW  physics rigid body + replicated block records
    VehicleOrientation.{h,cpp}    NEW  24-orientation rotation table + cell math
    VehicleModule.{h,cpp}         NEW  UVehicleModule + Thruster/Cockpit/Battery modules
    VehicleGridActor              DELETE
  Resources/
    ResourceNode.{h,cpp}          REDO server-authoritative mining, replicated integrity
  World/
    PlanetEnvironmentManager.{h,cpp} REDO replicated time-of-day + storm state + structure damage
  Save/
    ExoneerSaveGame.h             REDO records for pieces/vehicles
    SaveGameSubsystem.{h,cpp}     REDO enumerate BaseStructures + VehicleConstructs
  Player/
    PlayerSurvivalCharacter.{h,cpp} ADAPT tool modes, client UI RPC hook
    FirstPersonEngineerController   KEEP
```

`Exoneer.Build.cs`: add `NetCore` (FFastArraySerializer) to PublicDependencyModuleNames.

## 3. Replication model (applies to every system)

- Server (listen host) owns all state. Clients send intent via Server RPCs with
  `WithValidation`; the server revalidates distance, reachability, and resources.
- Replicated containers use `FFastArraySerializer` (inventory stacks, vehicle block
  records). Scalar state uses `DOREPLIFETIME` + RepNotify.
- Component RPC rule: a client can only call Server RPCs on components whose owning
  actor is owned by its connection (its own pawn). Therefore all client intent RPCs
  live on player-owned components (InteractionComponent, BuildToolComponent,
  MiningToolComponent, InventoryComponent for transfers) and take target actors or
  components as parameters.
- Cosmetic reactions (UI opening, highlight FX) never run on the server. The
  interaction pipeline calls `OnInteract` on the server and `OnInteractLocal` on the
  interacting client.
- Data asset references replicate by stable name through the package map. Entries are
  `TObjectPtr<UAssetClass>` at runtime, `TSoftObjectPtr` in authored data and saves.
- All replicated actors: `bReplicates = true` in constructor. `AVehicleConstruct`
  additionally `SetReplicatingMovement(true)`.

## 4. Shared types (ExoneerTypes.h)

```cpp
enum class EStructureTier : uint8 { Salvage, Alloy, Composite };
// Salvage: cheap, weak, storm-vulnerable. Alloy: standard. Composite: end-game.

enum class EMachineState : uint8 { Idle, Processing, OutputFull, LowPower };

enum class EConstructionPhase : uint8 { Ghost, UnderConstruction, Complete };

struct FInventoryEntry;          // stays in InventoryComponent.h (authoring/save struct)

struct FConstructionCost         // one stage of investment
{
    TArray<FInventoryEntry> Materials;   // consumed across the stage
    float WeldWork;                      // weld-seconds to finish the stage
};
```

## 5. Gameplay tags (ExoneerGameplayTags.h/.cpp, native tags)

```
Exoneer.Interaction.Use
Exoneer.Interaction.OpenContainer
Exoneer.Interaction.Pilot
Exoneer.Mount.Foundation      (piece mounts onto terrain or foundation edge sockets)
Exoneer.Mount.Wall
Exoneer.Mount.Floor
Exoneer.Mount.Ramp
Exoneer.Mount.Roof
Exoneer.Mount.Beam
Exoneer.Mount.Deployable      (machines mount onto floor/foundation surfaces)
Exoneer.Socket.*              (socket identity tags if needed by BP; optional)
```

Declared with `UE_DECLARE_GAMEPLAY_TAG_EXTERN` / `UE_DEFINE_GAMEPLAY_TAG`.

## 6. Inventory & interaction framework (already designed, part of this redo)

### UInventoryComponent
- Storage: `FInventoryList : FFastArraySerializer` containing
  `FInventoryStack : FFastArraySerializerItem { TObjectPtr<UItemDefinitionDataAsset> Item; int32 Count; }`.
- `TStructOpsTypeTraits<FInventoryList> { WithNetDeltaSerializer = true }`.
- Public API unchanged for callers: `AddItem`, `RemoveItem`, `GetItemCount`, `HasItems`,
  `ConsumeItems`, `GetCurrentLoad`, `GetLoadFraction`. `GetEntries()` now returns
  `TArray<FInventoryEntry>` **by value** (soft pointers built from runtime stacks).
- All mutations require `GetOwner()->HasAuthority()`; on a client they log and fail
  (AddItem returns full Count as "did not fit", RemoveItem returns 0, ConsumeItems false).
- Weight vs volume capacity model unchanged (`bUseWeight`, `MaxCapacity`; 0 = unlimited).
- Stack sizes from `UItemDefinitionDataAsset::MaxStack`, fill-existing-stacks-first.
- `OnInventoryChanged` broadcasts on server after mutation and on clients from fast
  array callbacks (`PostReplicatedAdd/Change`, `PreReplicatedRemove`).
- Container transfer: `RequestTransfer(Source, Target, Item, Count)` BlueprintCallable
  on the component. If authority, executes; otherwise sends `Server_RequestTransfer`
  (valid only when this component's owner is the calling connection's pawn). Server
  validates: both inventories alive, pawn within `TransferReach` (600 uu) of both
  owners, then moves with leftover-returns-to-source semantics.
- `MaxCapacity` and `bUseWeight` replicate so client UI computes load correctly.

### UInteractionComponent
- Focus trace stays client-side, tick only when the owning pawn is locally controlled.
  Focus gained/lost events remain cosmetic and local.
- `TryInteract()`: authority executes directly; remote client sends
  `Server_TryInteract(TargetActor)`. Server revalidates: target implements
  IInteractable, distance from pawn view point <= TraceDistance * 1.5 + target bounds
  radius. On success server calls `OnInteract(Target, Pawn)`, then
  `Client_InteractSucceeded(Target)` runs `OnInteractLocal` on the initiating client.
  Listen host runs both paths locally.

### IInteractable (redone)
- `OnFocusGained/OnFocusLost` (cosmetic, local),
- `OnInteract(AActor* Interactor)` – SERVER authoritative gameplay effect,
- `OnInteractLocal(AActor* Interactor)` – NEW, runs on the interacting client (open UI, FX),
- `GetInteractionPrompt()`,
- `GetInteractionTags()` – NEW, `FGameplayTagContainer` for HUD verb/icon filtering.

## 7. Base building (Icarus-style sockets)

### Data: UPieceDefinitionDataAsset
```cpp
FName PieceId;  FText DisplayName;  EStructureTier Tier;
FGameplayTag MountTag;               // what this piece is, for socket matching
TArray<FPieceSocketDef> Sockets;     // sockets this piece exposes once placed
TArray<FConstructionCost> Stages;    // ghost -> complete investment stages
float MaxHealth; float Mass;
int32 SupportBudget;                 // support units this piece passes downstream
int32 SupportCost;                   // support units this piece consumes from parent
bool bGroundable;                    // may snap to terrain and count as grounded
float StormResistance;               // 0..1 damage mitigation per tier baseline
TSoftObjectPtr<UStaticMesh> Mesh;  TSoftObjectPtr<UStaticMesh> GhostMesh;
TSubclassOf<ABasePiece> PieceClass;  // ABasePiece or AMachinePiece subclass
TSoftObjectPtr<UTexture2D> Icon;

struct FPieceSocketDef {
  FName SocketName;
  FTransform LocalTransform;           // where a mounted piece's origin lands
  FGameplayTagContainer AcceptedMounts;// which MountTags may snap here
};
```

### ABaseStructure (actor, replicated)
- Registry: `TArray<ABasePiece*> Pieces` (replicated array of actor refs is acceptable;
  pieces are actors and replicate individually; the registry replicates for client UI).
- Socket graph: server-side `TMap<FSocketKey, ABasePiece*> OccupiedSockets` where
  `FSocketKey { TWeakObjectPtr<ABasePiece> Piece; FName Socket; }`. Not replicated;
  clients ask the server via the build tool preview flow (see below) and derive
  visuals from replicated piece transforms.
- Hosts `UPowerNetworkComponent`.
- API (server): `CanPlacePiece(Def, ParentPiece, SocketName, out Error)`,
  `PlacePieceGhost(Def, ParentPiece, SocketName, PlacingPlayer) -> ABasePiece*`,
  `PlaceGroundedGhost(Def, Transform)`, `NotifyPieceRemoved(Piece)`,
  `RecomputeSupport()`.
- Support solver: BFS from grounded pieces across socket links. A piece's
  `SupportValue = max over linked neighbors (Neighbor.SupportValue - Piece.SupportCost)`,
  grounded pieces start at their `SupportBudget`. Placement requires resulting
  SupportValue > 0. On removal, recompute; pieces whose SupportValue drops <= 0 are
  destroyed after a short delay (batched collapse, server-side, with a multicast FX hook
  `OnPiecesCollapsed`). Full per-structure BFS on each change is acceptable at
  prototype scale (< 1000 pieces); optimize incrementally later.
- Structure merge: placing a piece whose snap parent belongs to structure A while
  overlapping a socket of structure B re-parents B's pieces into A (larger absorbs
  smaller). v2 keeps this simple: merge only via explicit socket link detection at
  placement time.

### ABasePiece (actor, replicated)
- Components: `UStaticMeshComponent Mesh`, `UConstructionComponent Construction`.
- Replicated: `Def (TObjectPtr<UPieceDefinitionDataAsset>)`, `Health`,
  `SupportValue`, `OwningStructure`.
- Implements IInteractable (prompt = display name), IDamageable, IConstructible.
- Health <= 0 -> structure removes piece (support recompute + possible collapse).
- Visual states driven by ConstructionComponent phase RepNotify: ghost material,
  construction material (progress scalar), final mesh.

### UConstructionComponent (on ABasePiece and AMachinePiece)
- Replicated: `Phase (EConstructionPhase)`, `StageIndex`, `StageProgress01`,
  plus `InvestedMaterials` (fast array reusing FInventoryStack) for refunds.
- Server API: `InvestWork(UInventoryComponent* Source, float WeldPoints)` – consumes
  missing stage materials from Source as progress passes material thresholds
  (materials are consumed proportionally across the stage), advances
  `StageProgress01 += WeldPoints * WorkPerWeldPoint / Stage.WeldWork`.
  Completing the last stage sets Phase=Complete and fires `OnConstructionComplete`.
- `Deconstruct(UInventoryComponent* Refund, float WreckPoints)` reverses progress and
  refunds invested materials (100% while ghost/under construction, 50% when complete).
- Ghost pieces: no collision vs pawns (overlap only), zero function (machines inert,
  no support contribution until Complete – ghosts have SupportCost 0 and provide 0).

## 8. Vehicle building (SE2-style unified grid)

### Data: UVehicleBlockDefinitionDataAsset
```cpp
FName BlockId;  FText DisplayName;
FIntVector SizeInCells;              // 25 cm cells, AABB occupancy
float Mass; float MaxHealth;
TArray<FConstructionCost> Stages;    // same staged investment as base pieces
TSubclassOf<UVehicleModule> ModuleClass;  // null for structural blocks
float PowerDelta;                    // + produces, - consumes (module blocks)
float EnergyStorage;                 // battery blocks
TSoftObjectPtr<UStaticMesh> Mesh;  TSoftObjectPtr<UTexture2D> Icon;
```

### AVehicleConstruct (actor, replicated, physics)
- Root: invisible `UStaticMeshComponent` (simple unit cube, no render), simulating
  physics on the server. `SetReplicatingMovement(true)`; clients receive replicated
  movement (no client physics).
- Per block: server + client spawn a `UBoxComponent` sized to the block AABB, attached
  to root (welded on server -> emergent mass/COM/inertia via per-box
  `SetMassOverrideInKg`; QueryOnly on clients) and register a visual instance in a per-
  mesh `UInstancedStaticMeshComponent` (no collision).
- Block records: `FVehicleBlockList : FFastArraySerializer` of
  `FVehicleBlockRecord : FFastArraySerializerItem {
     int32 BlockInstanceId; TObjectPtr<UVehicleBlockDefinitionDataAsset> Def;
     FIntVector Origin; uint8 Orientation;   // 0..23, table in VehicleOrientation.h
     float BuildProgress01; int32 StageIndex; float Health; float StateScalar; }`
  Fast array callbacks rebuild client boxes/instances incrementally.
- Cell math: `TMap<FIntVector, int32> CellToBlock` (server + client, rebuilt from
  records). `CanPlaceBlock(Def, Origin, Orientation)` = all cells free + at least one
  face-adjacent occupied cell (except the first block).
- Modules: for records whose Def has ModuleClass, the server instantiates a
  `UVehicleModule` (plain UObject, outer = construct) and ticks it from the construct's
  tick. Module state that clients need lives in the record (StateScalar) or on the
  construct (below). Modules of ghost/under-construction blocks are inert.
- Power ledger (simplified SE-style, server tick): sum production (solar/reactor
  modules), demand (thrusters by throttle, others by PowerDelta), charge/discharge
  battery records via StateScalar, compute `PowerSupplyFraction` (replicated float)
  that scales thrust.
- Piloting: `ACockpitModule` region – `IPilotable` implemented by AVehicleConstruct:
  `EnterPilot(APawn*, int32 CockpitBlockId)`. Replicated `PilotPawn`,
  `ActiveCockpitId`. Pilot pawn attaches to a seat socket transform derived from the
  cockpit block cell. Input: pilot's character forwards move/look via
  `Server_SetPilotInput(FVector Move, FVector Rotate)` RPC on the construct (pilot
  owns... NOTE: construct is NOT connection-owned; therefore the RPC lives on the
  PILOT's pawn/controller which calls a server-side method on the construct. Rule 3
  applies).
- Thrust: per thruster module, force = MaxThrust * throttle * PowerSupplyFraction
  along the module's world axis, applied at the module's world location
  (AddForceAtLocation). Throttle from desired local move vector alignment (existing
  v1 logic carried over). Torque: cockpit gyro model, AddTorqueInRadians scaled by mass.
- Split detection (server, on block removal/destruction): flood fill occupied cells
  from any remaining block; each disconnected island spawns a new AVehicleConstruct at
  the same world transform and its records move over; both rebuild. Single-block
  islands that are pure structure MAY instead drop as scrap items (tunable bool).
- Damage: blocks implement damage through the construct: `ApplyDamageToBlockAt(Cell,
  Amount, Type)`; Health <= 0 removes the block (drops partial refund scrap server-side).

### VehicleOrientation.h
- Static table of the 24 axis-aligned orientations as `FRotator`/`FQuat` +
  `RotateCellOffset(FIntVector, uint8 Orientation)` and inverse. Unit-tested by
  static_assert-style checks where possible (deterministic integer math).

## 9. Build tool (redo)

`UBuildToolComponent` on the player character. Modes:
- **BasePlacement:** hold a UPieceDefinitionDataAsset. Client ghost preview: trace,
  find ABasePiece under cursor -> enumerate its Def sockets whose AcceptedMounts
  contain held MountTag and that the client believes are free (client keeps a cheap
  socket-occupancy mirror derived from replicated piece transforms; final authority is
  the server), pick socket nearest the aim point, preview at socket transform. For
  `bGroundable` pieces with no socket candidate: terrain snap preview (surface normal
  align, slope limit). Confirm -> `Server_PlaceBasePiece(Def, ParentPiece, SocketName)`
  or `Server_PlaceGroundedPiece(Def, Transform)`. Server validates via
  ABaseStructure::CanPlacePiece (socket free, mount compatible, support > 0,
  no blocking overlap) and spawns the GHOST piece. Ghost placement costs the FIRST
  stage's first material entry? No – ghosts are free; materials flow during welding.
- **VehiclePlacement:** hold a UVehicleBlockDefinitionDataAsset. Trace against
  AVehicleConstruct -> hit cell + face -> preview at adjacent free cell; R cycles
  orientation. No construct hit and the block is a structural frame -> allow "found
  new construct" ghost on terrain. Confirm -> `Server_PlaceVehicleBlock(Construct,
  Def, Origin, Orientation)` / `Server_FoundVehicleConstruct(Def, Transform)`.
- **Weld mode (primary action held):** trace -> if target implements IConstructible,
  `Server_Weld(TargetActor, WorldPoint, DeltaWeldPoints)` at a fixed rate; server pulls
  materials from the player inventory via InvestWork/InvestInBlock and drains suit
  power. Secondary action = deconstruct (refund into player inventory).
- Preview validity events for HUD unchanged in spirit:
  `OnBuildPreviewChanged(bValid, EBuildPlacementError)`.
- `EBuildPlacementError { None, NoTarget, SocketOccupied, IncompatibleMount,
  NoSupport, BlockedByCollision, CellOccupied, NotAdjacent, OutOfReach, NotEnoughResources }`.

## 10. Machines (redo)

- `AMachinePiece : ABasePiece` adds `UPowerComponent`, `UInventoryComponent`,
  `UConveyorComponent`, replicated `EMachineState MachineState` (RepNotify ->
  `OnMachineStateChanged` BlueprintNativeEvent for VFX/audio).
- State computed server-side each crafting/power tick: LowPower if SupplyFraction <
  0.5, OutputFull if output stacks cannot fit, Processing if queue active, else Idle.
- Machines only function when Construction->Phase == Complete.
- `OnInteract` (server): reserved for gameplay effects (none for most machines).
  `OnInteractLocal` (client): `OpenMachineUI(User)` BlueprintNativeEvent -> UMG.
- Concrete: `ARefineryPiece`, `AFabricatorPiece`, `AOxygenGeneratorPiece` (each +
  `UCraftingComponent` with matching station type), `ABatteryPiece`, `ASolarPanelPiece`.
- `UCraftingComponent` redo: server-authoritative queue; replicate a compact queue
  summary (fast array of `{RecipeId, Progress01}`) for machine UI; progress scaling by
  power supply fraction retained.

## 11. Resources & world (redo)

### AResourceNode
- Replicated: `Integrity` (RepNotify -> mesh scale/FX on clients), `Def/Yield` fields.
- Mining: `UMiningToolComponent` client beam FX + trace; intent via
  `Server_MineTarget(Node, HitPoint)` (component on player pawn). Server validates
  range/rate (`MaxMineRange`, cooldown accumulator), applies damage, converts to yield
  into the OWNING player inventory (server), drains suit power on authority, destroys
  node at 0 with multicast FX hook.
- `YieldMultiplier` per node instance for deposit richness variants.

### APlanetEnvironmentManager
- Replicated: `TimeOfDay01` (float, clients interpolate), `bStormActive` +
  `StormIntensity` (RepNotify -> local wind/audio/post FX).
- Server: storm ticks apply damage to exposed COMPLETE base pieces
  (`Damage * (1 - StormResistance)`), cadence 1/s while storm active. Exposure check
  v2: upward trace from piece bounds top; cache per piece, invalidate on structure
  change. Solar production multiplier = f(sun fraction, storm).

## 12. Save (redo)

- `FSavedBasePiece { FName PieceId; FTransform WorldTransform; uint8 Phase;
  int32 StageIndex; float StageProgress01; float Health;
  TArray<FInventoryEntry> Inventory; float StoredEnergy; }`
- `FSavedStructure { TArray<FSavedBasePiece> Pieces; }` – socket graph is re-derived
  on load by proximity re-link (transforms are exact; sockets whose world transforms
  coincide within 1 cm re-link).
- `FSavedVehicle { FTransform Transform; TArray<FSavedVehicleBlock> Blocks; }` with
  `FSavedVehicleBlock { FName BlockId; FIntVector Origin; uint8 Orientation;
  int32 StageIndex; float BuildProgress01; float Health; float StateScalar; }`
- Defs resolve via AssetManager primary asset ids (`Piece:<PieceId>`, `VehicleBlock:<BlockId>`).
- Save only on server; load recreates world server-side (replication rebuilds clients).

## 13. Player integration

- Tool modes: Mine / Build / Weld (Q cycles). Weld is **construction**
  (ghost → complete). v2 still contains a weld-to-`RepairHealth` path on
  Complete parts; GAME-SCOPE §10 flags it as illegal. v3 deletes that path
  when wheels become swappable (ROADMAP P3).
- `Client_InteractSucceeded` flow lives in UInteractionComponent (section 6).
- Character survival internals untouched this pass; survival stat drains triggered by
  tools apply only on authority.

## 14. Sequencing (implementation order)

1. Core: Build.cs, ExoneerTypes.h, ExoneerGameplayTags, Interfaces.
2. Data assets: Piece/VehicleBlock definitions, biome additions.
3. Inventory + Interaction (contract for everything).
4. ConstructionComponent.
5. Building: BasePiece, BaseStructure (sockets + support).
6. BuildTool + MiningTool.
7. Vehicles: orientation table, VehicleConstruct, modules.
8. Machines + crafting/conveyor/power redo.
9. Resources + world.
10. Save.
11. Player adaptation + README architecture section update.

## 15. Verification

v2 was verified by review against UE 5.8 APIs and by subsequent local
editor builds (wheels, terramechanics tests). Treat compiler output and
`Exoneer.Terramechanics` automation as the living arbiter. v3 verification
is the matrix in [ROADMAP.md](ROADMAP.md).

---

## 16. Condition (v3, not implemented)

Spec: [design/maintenance.md](design/maintenance.md). Rules: GAME-SCOPE §10.

Immediate damage stays on existing `Health` / `FVehicleBlockRecord::Health`.
Condition is a **second replicated payload**, never a 0–1 durability alias.

### 16.1 Shared readings

```cpp
USTRUCT(BlueprintType)
struct FPartCondition
{
    GENERATED_BODY()
    // Tire (mm, C, kPa). 0-depth is terminal for a tire.
    float TreadDepthMm = -1.f;       // <0 = N/A for this part class
    float CarcassTempC = 0.f;
    float InflationKPa = 0.f;
    // Motor
    float WindingTempC = 0.f;
    // Battery / tank
    float CapacityFade01 = 0.f;      // 0 = nominal capacity, 1 = dead
    // Exposed faces (solar, radio)
    float SurfaceOpacity01 = 0.f;    // dust; production *= (1 - opacity)
    // Seals
    float LeakRateLps = 0.f;
    float DeflectionMm = 0.f;
};
```

Part classes fill only their fields; UI skips N/A (`TreadDepthMm < 0`).

### 16.2 Where it lives

- **Vehicle blocks:** extra fields on `FVehicleBlockRecord` (or a parallel
  fast array keyed by `BlockInstanceId` — prefer fields on the record so
  one delta carries health + condition). Wheel telemetry already has a
  side array; fold tread/temp into that or into the record, not a third
  channel.
- **Base pieces:** `FPartCondition` on `ABasePiece`, replicated. Solar
  opacity is the 1.0 teaching case.
- **Pawn suit:** seal leak on `USurvivalStatsComponent` (not a new actor).

Spend functions are server-only, SI, called from the systems that already
measure the cause (`UWheelModule` / sim telemetry for slip×load and
copper loss; `APlanetEnvironmentManager` storm tick for exposure;
`ABaseStructure::RecomputeSupport` for overload-hours).

### 16.3 Repair routing (build tool)

`UBuildToolComponent` weld on Complete:

- If the target is a **ghost / under construction** → `InvestConstruction` (unchanged).
- If the target is Complete and the failure is a **weldable metal crack** →
  patch Health only when a future crack flag exists.
- If the target is Complete **tire / motor / solar / battery** → **do not
  weld.** Primary on a worn wheel with a spare in inventory is **Replace**
  (consume item, reset that record's condition, refund scrap optionally).
- Delete `RepairHealth` as a generic heal.

Replace is a Server RPC on the build tool (player-owned), same validation
pattern as weld (range, authority, inventory).

### 16.4 Save

Extend `FSavedVehicleBlock` and `FSavedBasePiece` with `FPartCondition`
(or the subset of floats). Tagged UPROPERTY serialization keeps old saves
defaulting to nominal (full tread = spec radius-derived new-tire depth).

---

## 17. Optional projects (v3, not implemented)

Spec: [design/projects.md](design/projects.md).

### 17.1 Data

`UProjectDefinitionDataAsset` : `UPrimaryDataAsset` with
`PrimaryAssetType = "Project"`. Scan `/Game/Exoneer/Data/Projects`.
Register a sixth `+PrimaryAssetTypesToScan` line in `DefaultGame.ini`.

`FProjectCriterion`: enum type + float target (SI) + optional hold
duration in sols. Evaluator reads live systems; no quest tokens.

### 17.2 Runtime

`UProjectSubsystem` : `UWorldSubsystem` (or GameState component). Server
simulates; a compact `TArray<FProjectRuntime>` replicates on GameState
(`bReplicates`).

Accept / Abandon: player-owned Server RPC on the character or a wrist
component (constructs are not connection-owned — same rule as §3).

Tick ≤ 1 Hz. Long Watch / Handshake / Road to Orbit are three data
assets, not three C++ subclasses.

### 17.3 Knowledge rewards

Handshake writes `FOrbitalKnowledge` (window times, pad constraints) into
the save and onto the wrist. It does not grant items or unlock pieces.

---

## 18. Persistence expansions (v3)

v2 already saves player, structures, vehicles, time-of-day
(`ExoneerSaveGame.h`). v3 adds, all server-side:

| Field | Why |
|---|---|
| `FPartCondition` per piece and per block | Maintenance survives load |
| Storm state (`bStormActive`, intensity, next named-storm sol) | Long Watch soak |
| `TArray<FProjectRuntime>` | Optional projects |
| `FOrbitalKnowledge` | Handshake payload |
| Suit leak rate | Seal condition |

Load path stays “clear built world, respawn via placement APIs, restore
scalars.” Restore condition **after** records exist (same as
`RestoreVehicleBlockRecord` today).

Join-in-progress: no extra channel if condition rides piece/block
replication and projects ride GameState. Verify with ROADMAP V-JIP.

Umbilical, dish actor, fuel module, and road/bridge pieces are content +
small classes on this layer; they do not replace the v2 spine.
