# Exoneer wheels pass – content, input, test-world, and verification plan

Scope of this document: the production/content half of the wheels-with-Bekker-Wong pass. It consumes the three exploration reports (agent0 terrain, agent1 pipeline, agent2 runtime) and pins every content decision the runtime plan needs to compile against. All names below are contracts: the C++ plan must use these exact UPROPERTY names, because the bootstrap wires assets to CDO properties by string.

No math-agent soil table exists in the scratchpad, so soil values below are taken directly from the published source: J.Y. Wong, *Theory of Ground Vehicles*, pressure-sinkage and shear parameter tables (LLL dry sand, LLL sandy loam, Thailand clayey soil, U.S. snow). They are cited per asset so anyone can re-check them.

---

## 0. Decisions made in this plan (summary)

| Question | Decision | Why |
|---|---|---|
| Wheel block size | 3x1x3 cells (75 x 25 x 75 cm AABB), tire radius 0.35 m, width b 0.25 m | 70 cm diameter sits in the 60–80 cm target band, leaves 2.5 cm AABB clearance for tire bulge; 2 cells (50 cm) gives too short a contact patch for the flat-plate Bekker approximation on a ~900 kg rover; width 1 cell keeps b = 0.25 m, a real light-rover tire width |
| One wheel block or two | Two: drive wheel (fixed) and steer wheel (steering rack fitted) | The steering rack is a physical difference, not a mode flag – matches the section 7 realism rule |
| Steer/throttle input | Reuse IA_Move in ground mode (W/S throttle, A/D steer); no new axis assets | Zero RPC churn; IA_Look frees up for camera in ground mode (runtime plan owns that switch) |
| Handbrake | New IA_Handbrake, bool, LeftControl | Free key, latching press handled server-side |
| CTIS | Two bool actions: IA_TirePressureDown on G, IA_TirePressureUp on H; no AXIS1D constant needed | A CTIS is a slow pump – holding a key at a fixed kPa/s rate matches the physical process; mouse wheel stays free and discrete wheel ticks map badly to a continuous pump |
| Soil physical materials authored in v1 | PM_Soil_DrySand and PM_Soil_Clay | Sand demonstrates the sinkage/CTIS problem, clay demonstrates the shear/traction problem (low phi); loam and snow are listed as data, not authored |
| Surface identification | UExoneerSoilPhysicalMaterial subclass resolved from FHitResult::PhysMaterial; no EPhysicalSurface enum entries | The subclass carries the Bekker constants directly, so the enum adds bookkeeping and no information |
| Wheel probe channel | New trace channel WheelProbe (ECC_GameTraceChannel1), default response Block | ECC_Visibility is poisoned by ghost boxes (they block it by design); default-Block means stock ground actors need no edits, and the C++ side explicitly ignores self and ghost boxes |

Predicted behavior of the test rover (~840 kg, four wheels, W ~ 2.06 kN per wheel), from the section 4.1 formulas with the Wong values below – these same numbers seed the automation tests and the playtest expectations:

- Dry sand at nominal 220 kPa: z ~ 17 cm, compaction resistance ~ 4.6 kN per wheel – the rover bogs within a length. This is correct Bekker behavior, not a bug.
- Dry sand at 80 kPa (CTIS dropped): z ~ 6.8 cm, R ~ 0.65 kN per wheel (2.6 kN total) vs peak tractive effort ~ 4.5 kN total – drives with margin, digs in if slip runs to 1.0.
- 10 degree sand slope at 80 kPa: grade force 1.43 kN + R 2.6 kN ~ 4.0 kN vs 4.5 kN available – climbs only near the optimal slip window. 20 degrees: 5.4 kN needed – cannot climb. The 10-vs-20 checklist contrast falls straight out of the math.
- Clay at 80 kPa: z ~ 1.2 cm, R ~ 0.6 kN total, but peak F only ~ 2.3 kN (phi = 13 degrees) – drives, feels slippery, marginal on 10 degrees.

---

## 1. BootstrapPrototype.py changes

All edits preserve the script's contract: idempotent, exists-or-create, properties re-applied unconditionally on every run. File: `c:/Users/Mark/Documents/GitHub/Exoneer/scripts/BootstrapPrototype.py`.

### 1.1 ensure_block – open the prop seam (line 316)

Mirror the `ensure_piece` pattern (`machine=` merge at :260-261):

```python
def ensure_block(name, block_id, display, mesh, stages, module=None, mass=50.0,
                 health=200.0, power=0.0, storage=0.0, thrust=0.0,
                 size_in_cells=(1, 1, 1), extra=None):
    ...
    "size_in_cells": unreal.IntVector(*size_in_cells),
    ...
    if extra:
        props.update(extra)
    set_props(asset, props)
```

Existing five call sites (:346-362) are untouched – both new parameters default to current behavior.

### 1.2 Wheel spec through the extra seam

The runtime plan adds `FVehicleWheelSpec` (must be `USTRUCT(BlueprintType)` so Python can construct `unreal.VehicleWheelSpec`) plus two flat properties on `UVehicleBlockDefinitionDataAsset`. Name contract, chosen so the Python snake_case mangling is unambiguous (b-prefix bools drop the b; avoid mixed-cap acronyms like "KPa"):

| C++ UPROPERTY | Python name | Test rover value | Unit |
|---|---|---|---|
| `WheelRadiusM` | `wheel_radius_m` | 0.35 | m |
| `TireWidthM` | `tire_width_m` | 0.25 | m |
| `NominalPressureKpa` | `nominal_pressure_kpa` | 220.0 | kPa |
| `MinPressureKpa` | `min_pressure_kpa` | 60.0 | kPa (debead floor) |
| `MaxPressureKpa` | `max_pressure_kpa` | 350.0 | kPa |
| `MaxDriveTorqueNm` | `max_drive_torque_nm` | 180.0 | N·m (in-hub electric motor) |
| `MaxBrakeTorqueNm` | `max_brake_torque_nm` | 400.0 | N·m |
| `MaxAngularSpeedRadPerSec` | `max_angular_speed_rad_per_sec` | 25.0 | rad/s (~31 km/h at r = 0.35) |
| `MaxSteerAngleDeg` | `max_steer_angle_deg` | 35.0 (steer) / 0.0 (drive) | deg |
| `SpringRateNewtonPerMeter` | `spring_rate_newton_per_meter` | 30000.0 | N/m (~30% static compression of travel) |
| `DamperRateNewtonSecPerMeter` | `damper_rate_newton_sec_per_meter` | 2800.0 | N·s/m (zeta ~ 0.6 at 182 kg corner mass) |
| `SuspensionRestLengthM` | `suspension_rest_length_m` | 0.30 | m |
| `SuspensionTravelM` | `suspension_travel_m` | 0.20 | m |
| `bSteerable` | `steerable` | True / False | – |
| `bDriven` | `driven` | True | – |

Struct round-trip rule: Python receives copies, so build a fresh struct each run and assign the whole thing (same reason as the IMC modifier round-trip at :153-166):

```python
def make_wheel_spec(steerable):
    s = unreal.VehicleWheelSpec()
    for prop, val in [("wheel_radius_m", 0.35), ("tire_width_m", 0.25),
                      ("nominal_pressure_kpa", 220.0), ("min_pressure_kpa", 60.0),
                      ("max_pressure_kpa", 350.0), ("max_drive_torque_nm", 180.0),
                      ("max_brake_torque_nm", 400.0), ("max_angular_speed_rad_per_sec", 25.0),
                      ("max_steer_angle_deg", 35.0 if steerable else 0.0),
                      ("spring_rate_newton_per_meter", 30000.0),
                      ("damper_rate_newton_sec_per_meter", 2800.0),
                      ("suspension_rest_length_m", 0.30), ("suspension_travel_m", 0.20),
                      ("steerable", steerable), ("driven", True)]:
        s.set_editor_property(prop, val)
    return s
```

### 1.3 Wheel block definitions

Two blocks, section 4 of the script, after `blocks["solar"]`:

```python
wheel_stage = [make_stage([make_entry(items["tire"], 1),
                           make_entry(items["wheel_hub"], 1),
                           make_entry(items["motor"], 1)], 3.0)]
blocks["wheel_drive"] = ensure_block(
    "DA_Block_WheelDrive", "wheel_drive", "Drive Wheel", CYLINDER, wheel_stage,
    module=module_class("WheelModule"), mass=45, health=250, power=-50.0,
    size_in_cells=(3, 1, 3),
    extra={"wheel_spec": make_wheel_spec(False),
           "ignore_terrain_overlap_on_place": True})
blocks["wheel_steer"] = ensure_block(
    "DA_Block_WheelSteer", "wheel_steer", "Steer Wheel", CYLINDER, wheel_stage,
    module=module_class("WheelModule"), mass=48, health=250, power=-50.0,
    size_in_cells=(3, 1, 3),
    extra={"wheel_spec": make_wheel_spec(True),
           "ignore_terrain_overlap_on_place": True})
```

Notes:
- `module_class("WheelModule")` uses the existing `/Script/Exoneer.` reflection path (:338-342); the C++ class must be concrete, not Abstract.
- `bIgnoreTerrainOverlapOnPlace` (python `ignore_terrain_overlap_on_place`) is the per-definition placement allowance from agent2 obstacle 5/blocker 8. It serves both interactive placement (a wheel legitimately sits at ground level) and the save/load ghost-replay path, so parked-in-sand rovers stop silently losing wheels.
- Mesh is the engine CYLINDER placeholder. It is Z-aligned while a wheel spins about block-local Y; the per-wheel `UStaticMeshComponent` the runtime plan creates in `RebuildDerivedState` should bake a constant 90 degree roll and a radius/width scale when it creates the component. Do not add a data field for this yet – one constant in one function, replaced when a real wheel mesh is authored.
- Mass coherence: tire 18 + hub 15 + motor 8 ~ 41 kg parts for a 45 kg block – close enough to be honest.
- `power=-50` is idle draw (electronics + CTIS compressor idle). Traction power is dynamic: the module's `GetCurrentDraw` returns torque x wheel speed / efficiency, so sand crossings visibly drain batteries. That is intended survival pressure, not a bug.

### 1.4 Items and recipes

`item_specs` (:185-199), two additions:

```python
"tire":      ("Rover Tire", CAT.COMPONENT, 20, 18.0, 3.0),
"wheel_hub": ("Wheel Hub Assembly", CAT.COMPONENT, 20, 15.0, 1.5),
```

`recipe_specs` (:370-377), two additions:

```python
("recipe_tire", "Mold Rover Tire", STATION.FABRICATOR,
    [("carbon", 4), ("silicon_wafer", 1)], [("tire", 1)], 6.0, 300.0),
("recipe_wheel_hub", "Machine Wheel Hub", STATION.FABRICATOR,
    [("iron_ingot", 2), ("plate", 1)], [("wheel_hub", 1)], 5.0, 250.0),
```

(Synthetic elastomer from carbon feedstock with silica filler is real tire chemistry – it keeps the crafting chain honest and uses the existing carbon node.)

### 1.5 Quick bar and starter items

`quick_bar` (:436-442): append `blocks["wheel_drive"], blocks["wheel_steer"]`. Without this the blocks are unreachable in game (B cycles the quick bar; there is no other selection UI).

`starter_items` (:443-450): append `make_entry(items["tire"], 8), make_entry(items["wheel_hub"], 8)` so Mark's first rover needs no crafting round-trip. Existing 10 motors and 30 plates cover the rest.

### 1.6 Soil physical materials

New folder constant `DIR_PHYSMATS = ROOT + "/PhysicalMaterials"` and a helper – physical materials are not data assets, so `ensure_data_asset` does not cover them:

```python
def ensure_phys_material(name, klass):
    full = DIR_PHYSMATS + "/" + name
    if eal.does_asset_exist(full):
        return unreal.load_asset(full)
    factory = unreal.PhysicalMaterialFactoryNew()
    factory.set_editor_property("physical_material_class", klass)
    asset = asset_tools.create_asset(name, DIR_PHYSMATS, klass, factory)
    if asset is None:
        raise RuntimeError("Failed to create physical material " + full)
    return asset

soil_class = unreal.load_class(None, "/Script/Exoneer.ExoneerSoilPhysicalMaterial")
```

(Use `load_class` even if the class is BlueprintType – same robustness reasoning as `module_class`.)

C++ field name contract on `UExoneerSoilPhysicalMaterial` (python names in parentheses): `BekkerKc` (`bekker_kc`, kN/m^(n+1)), `BekkerKphi` (`bekker_kphi`, kN/m^(n+2)), `BekkerN` (`bekker_n`), `CohesionKpa` (`cohesion_kpa`), `FrictionAngleDeg` (`friction_angle_deg`), `ShearDeformationK` (`shear_deformation_k`, m).

Authored in v1 (values from Wong's published tables):

```python
pm_sand = ensure_phys_material("PM_Soil_DrySand", soil_class)
set_props(pm_sand, {"bekker_kc": 0.99, "bekker_kphi": 1528.43, "bekker_n": 1.1,
                    "cohesion_kpa": 1.04, "friction_angle_deg": 28.0,
                    "shear_deformation_k": 0.025})   # LLL dry sand
pm_clay = ensure_phys_material("PM_Soil_Clay", soil_class)
set_props(pm_clay, {"bekker_kc": 13.19, "bekker_kphi": 692.15, "bekker_n": 0.5,
                    "cohesion_kpa": 4.14, "friction_angle_deg": 13.0,
                    "shear_deformation_k": 0.01})    # Thailand clayey soil
```

Noted for later, not authored in this pass: sandy loam (kc 5.27, kphi 1515.04, n 0.7, c 1.72 kPa, phi 29, K 0.025) and snow (kc 4.37, kphi 196.72, n 1.6, c 1.03 kPa, phi 19.7, K 0.04). Any hit whose PhysMaterial is not the soil subclass resolves as rigid ground – the default slab needs no asset.

These are plain referenced assets (reached through the material on the mesh at trace time), not id-resolved primary assets – no DefaultGame.ini change is needed for them.

### 1.7 Ground material instances

Materials carry the PhysMaterial, meshes do not. Two `MaterialInstanceConstant` assets under `/Game/Exoneer/Materials`, parented to the engine basic-shape material:

```python
DIR_MATS = ROOT + "/Materials"
BASE_MAT = unreal.load_asset("/Engine/BasicShapes/BasicShapeMaterial")

def ensure_ground_mic(name, color, phys_mat):
    full = DIR_MATS + "/" + name
    if eal.does_asset_exist(full):
        mic = unreal.load_asset(full)
    else:
        factory = unreal.MaterialInstanceConstantFactoryNew()
        factory.set_editor_property("initial_parent", BASE_MAT)
        mic = asset_tools.create_asset(name, DIR_MATS, unreal.MaterialInstanceConstant, factory)
    unreal.MaterialEditingLibrary.set_material_instance_vector_parameter_value(
        mic, "Color", unreal.LinearColor(*color))
    mic.set_editor_property("phys_material", phys_mat)
    return mic

mi_sand = ensure_ground_mic("MI_Ground_DrySand", (0.76, 0.65, 0.42, 1.0), pm_sand)
mi_clay = ensure_ground_mic("MI_Ground_Clay",    (0.45, 0.28, 0.20, 1.0), pm_clay)
```

Authored, colored materials make the fields readable at a glance in playtest video.

### 1.8 New input actions

`action_specs` (:105-111), three additions – bool only, no AXIS1D constant needed:

```python
"IA_Handbrake": BOOL, "IA_TirePressureDown": BOOL, "IA_TirePressureUp": BOOL,
```

`BINDINGS` (:125-143), three additions:

```python
("IA_Handbrake", "LeftControl", []),
("IA_TirePressureDown", "G", []),
("IA_TirePressureUp", "H", []),
```

Contract with C++: `APlayerSurvivalCharacter` gains three UPROPERTYs named exactly `IA_Handbrake`, `IA_TirePressureDown`, `IA_TirePressureUp` (the CDO wiring loop at :434-435 is blind – a name mismatch throws at bootstrap). The bootstrap must therefore run only after the C++ that declares them compiles; the rollout order in section 7 enforces this. Steer and throttle add nothing here: IA_Move is reused in ground mode. CTIS keys are hold-to-pump; a sensible server pump rate is 20 kPa/s (220 to 90 kPa in ~6.5 s). Handbrake presses must be latched server-side because the 20 Hz batch zeroes local intent between sends – that is a runtime-plan note, recorded here so nobody trims it.

---

## 2. The map problem – unconditional ensure-actor pass

The level branch at :468-473 is create-only, so nothing added inside the `else` will ever appear in the existing `L_StarterPlanet`. Add a new pass that runs unconditionally after the load/create branch and before the final save, built on labels:

```python
def ensure_actor(label, cls, loc, rot=(0, 0, 0), scale=(1, 1, 1)):
    found = None
    for a in eas.get_all_level_actors():
        if a.get_actor_label() == label:
            found = a
            break
    if found is None:
        found = eas.spawn_actor_from_class(cls, unreal.Vector(*loc),
                    unreal.Rotator(roll=rot[0], pitch=rot[1], yaw=rot[2]))
        found.set_actor_label(label)
        log("spawned " + label)
    found.set_actor_location(unreal.Vector(*loc), False, False)
    found.set_actor_rotation(unreal.Rotator(roll=rot[0], pitch=rot[1], yaw=rot[2]), False)
    found.set_actor_scale3d(unreal.Vector(*scale))
    return found

def ensure_test_slab(label, loc, rot, scale, mic):
    actor = ensure_actor(label, unreal.StaticMeshActor, loc, rot, scale)
    mesh_comp = actor.get_editor_property("static_mesh_component")
    mesh_comp.set_editor_property("static_mesh", CUBE)
    mesh_comp.set_material(0, mic)
    return actor
```

Properties are re-applied every run (same idempotency contract as the assets), so transform tuning later propagates to existing maps. End the pass with `les.save_current_level()` unconditionally.

Geometry facts this layout respects: the ground slab is 400 x 400 m with its top at Z = -2 (not 4 km, not Z = 0 – the old comment at :478 is wrong and should be corrected while in there). The engine cube is 100 uu. Resource nodes occupy roughly the ring 900–2000 uu from origin; the test range sits east of X = 2000, inside the slab edge at 20000.

### Test range actors

All StaticMeshActors, engine CUBE mesh. "Top flush at -2" means center Z = -2 minus half the scaled height.

| Label | Class/material | Location (uu) | Rotation | Scale | Purpose |
|---|---|---|---|---|---|
| `TestField_Sand` | MI_Ground_DrySand | (6000, 2000, -27) | – | (80, 60, 0.5) | 80 x 60 m dry sand field, top flush at Z = -2; the main sinkage/CTIS arena |
| `TestField_Clay` | MI_Ground_Clay | (6000, -4000, -27) | – | (80, 40, 0.5) | 80 x 40 m clay field; the low-phi traction arena |
| `TestRamp_10deg` | MI_Ground_DrySand | (10500, 3500, 191) | pitch +10 | (25, 10, 0.4) | 25 m sand-surfaced ramp rising east off the sand field; lower edge buried ~4 uu below grade so there is no lip |
| `TestRamp_20deg` | MI_Ground_DrySand | (10500, 500, 403) | pitch +20 | (25, 10, 0.4) | same, 20 degrees – the "cannot climb" contrast |
| `TestPit_Curb_N` | default material | (4000, 4200, 8) | – | (14, 0.4, 0.5) | hard curb, top ~35 cm above sand |
| `TestPit_Curb_S` | default material | (4000, 2800, 8) | – | (14, 0.4, 0.5) | hard curb |
| `TestPit_Curb_E` | default material | (4700, 3500, 8) | yaw 90 | (14, 0.4, 0.5) | hard curb; west side open toward spawn |

On the pit: the slab is a solid cube, so a geometric depression cannot be cut into it without new meshes, and in this model sinkage *is* the depth. The "shallow pit" is therefore a 14 x 14 m curbed bay inside the sand field: drive in through the open west side, bog down at full throttle, and the ~35 cm curbs (one wheel radius) make casual exits impossible – recovery is reverse-out with managed slip and dropped pressure, which is exactly the CTIS exercise. If a true depression is wanted later, it comes with the PCG/landscape work, out of this pass.

Positioning sanity: the sand field spans X 2000–10000, Y -1000–5000; the clay field X 2000–10000, Y -6000–-2000; both clear of all six resource nodes. Ramp lower edges start on the sand field (X ~ 9270) so every climb is approached over soft soil. Spawn to sand edge is 20 m of hard slab – the baseline drive.

Ramp center-height derivation, for whoever tunes them: for scale (25, 10, 0.4) the half-length is 1250 uu and half-thickness 20 uu; lower-edge top corner sits at `centerZ - 1250*sin(pitch) + 20*cos(pitch)`; set that ~4 uu below -2. That yields 191 for 10 degrees and 403 for 20 degrees.

Also correct while in the map section: the two wrong comments at :478 ("4 km", "top at Z = 0").

---

## 3. Config edits

### Config/DefaultEngine.ini

Physics block (lines 18–22) – fix the dead key and tighten substeps for the wheel solver (final values subject to the runtime plan, but the key rename is unconditional):

```ini
[/Script/Engine.PhysicsSettings]
DefaultGravityZ=-980.0
bSubstepping=True
MaxSubsteps=6
MaxSubstepDeltaTime=0.008333
```

`PhysicSubstepDeltaTime` is not a recognized UPhysicsSettings property – it has silently fallen back to the engine default since day one. `MaxSubstepDeltaTime=1/120` with `MaxSubsteps=6` keeps full-rate substepping down to ~20 fps, which the per-substep `AddCustomPhysics` suspension needs.

New section for the wheel probe channel:

```ini
[/Script/Engine.CollisionProfiles]
+DefaultChannelResponses=(Channel=ECC_GameTraceChannel1,DefaultResponse=ECR_Block,bTraceType=True,bStaticObject=False,Name="WheelProbe")
```

Default Block means the stock-profile ground slab and test slabs block it with no per-actor edits. The C++ side must: ignore self via `FCollisionQueryParams(SCENE_QUERY_STAT(ExoneerWheelProbe), false, Construct)`, set the ghost-box response to ECR_Ignore on this channel (ghost boxes already hand-build their response set at VehicleConstruct.cpp:1344-1348), and set `bReturnPhysicalMaterial = true` on the query.

No `+PhysicalSurfaces=` entries: the soil subclass carries the constants, the enum would carry nothing.

### Config/DefaultInput.ini

Update the stale comment block (lines 1–27): add the three new IA lines and a note that ground-mode steering/throttle reuse IA_Move, and fix the documented-but-wrong cancel key while there (script binds X, README says Esc – the comment should match the script):

```ini
;   IA_Handbrake          Digital (hold, LeftControl) - ground vehicles
;   IA_TirePressureDown   Digital (hold, G) - CTIS deflate
;   IA_TirePressureUp     Digital (hold, H) - CTIS inflate
;   Ground vehicles: IA_Move doubles as throttle (Y) and steer (X) while piloting.
```

### Config/DefaultGame.ini

No change. Wheel blocks ride the existing `VehicleBlock` primary asset type; soil physical materials are hard-referenced by the ground materials and never resolved by id.

---

## 4. Visor HUD interim pilot panel

Scaffolding only – GAME-SCOPE section 8 mandates diegetic dashboards later, so the HUD draws from public accessors only and owns no vehicle state. Files: `Source/Exoneer/Player/ExoneerHUD.h/.cpp`.

Data contract (runtime plan provides; recorded here so the two plans meet): a per-construct replicated summary, one payload per vehicle regardless of wheel count, quantized and updated at low rate outside `FVehicleBlockRecord` so it never triggers visual rebuilds:

```cpp
USTRUCT(BlueprintType) struct FVehicleDrivetrainSummary {
    float WorstSlip;        // highest slip ratio across driven wheels
    float MaxSinkageM;      // deepest wheel sinkage, meters
    float TirePressureKpa;  // current CTIS setpoint
    int32 WheelCount;       // 0 = not a wheeled construct
};
// BlueprintPure accessor on AVehicleConstruct:
const FVehicleDrivetrainSummary& GetDrivetrainSummary() const;
```

Speed comes from `Construct->GetVelocity()` (movement already replicates) and power from the already-replicated `PowerSupplyFraction` – neither goes in the struct.

HUD changes:
- `AExoneerHUD::Tick`: smooth `SmoothedWorstSlip` via `FMath::FInterpTo(..., 4.f)` next to the existing `SmoothedSuitDrain` pattern (ExoneerHUD.cpp:38-54). Slip is the bounciest number on the panel; sinkage and pressure draw raw.
- New `void DrawPilotPanel(const APlayerSurvivalCharacter*)` declared with the other draws, called from `DrawHUD` after `DrawVitals`, guarded on `Engineer->IsPiloting()` and `GetDrivetrainSummary().WheelCount > 0` (thruster-only craft keep today's HUD). Panel anchors bottom-center, X = SizeX * 0.5 - 130, Y = SizeY - 150, using `DrawReadout` rows:

| Row | Format | Color rule |
|---|---|---|
| `== DRIVE ==` | header | VisorDim |
| SPEED | `%.1f m/s  (%.0f km/h)` | VisorMain |
| SLIP | `%.2f   opt 0.15-0.25` | VisorGood inside 0.15–0.25, VisorMain below 0.15, VisorWarn above 0.35 |
| SINKAGE | `%.1f cm` | VisorWarn above 40% of wheel radius (14 cm for this wheel) |
| TIRE | `%.0f kPa` | VisorWarn outside 80–300; append `(pumping)` while G/H held, using the 1.2 s freshness-gate pattern from the weld feedback (:180) |
| DRIVE PWR | `%.0f%%` from PowerSupplyFraction | VisorWarn below 100% |

The optimal-slip window is printed on the row itself because the whole Shear Control talent (scope 7) is about holding that window – Mark needs to see it to learn the mechanic before the talent exists.

C4458 reminder for whoever writes it: no locals named `Tags` or `Instigator`, and the summary accessor cannot be named `IsActive`/`SetActive`.

---

## 5. First test rover

Recipe for Mark's playtest – all parts are in starter items, no crafting needed.

Block list and mass:

| Blocks | Count | Mass each | Total |
|---|---|---|---|
| Frame (chassis, 2 wide x 4 long, one layer) | 8 | 40 | 320 |
| Cockpit (front center, on top of chassis) | 1 | 120 | 120 |
| Small battery | 2 | 80 | 160 |
| Solar collector | 2 | 30 | 60 |
| Steer wheel (front pair) | 2 | 48 | 96 |
| Drive wheel (rear pair) | 2 | 45 | 90 |
| **Total** | **17** | | **~846 kg** |

Two batteries are deliberate: soft-soil driving at ~10 kW drains one 900 kJ battery in about 90 seconds, and running dry mid-sand is a lesson for a later session, not the first one. No thrusters on the test rover – thruster force would mask traction loss and the gyro torque would fake slope authority; the terramechanics signal stays clean. (The thruster block stays in the quick bar and existing hover craft are untouched.)

Build sequence in game, on the hard slab near spawn:

1. Press B until a vehicle block is selected, Q to Build mode.
2. Place the 8 frame ghosts as a 2 x 4 deck, one cell above comfortable wheel height – wheels are 3 cells (75 cm) tall, so the deck sits at roughly waist height. Nothing simulates while everything is a ghost, so the deck hangs in place.
3. Place the cockpit ghost on top of the front-center frame.
4. Place 2 battery ghosts and 2 solar ghosts on the remaining deck top.
5. Place the 4 wheel ghosts against the deck's outer sides: steer wheels on the front corners, drive wheels on the rear corners. The `ignore_terrain_overlap_on_place` flag on wheel definitions makes ground proximity legal.
6. Q to Weld mode, weld every block to Complete. When the first block completes, physics starts and the construct settles onto its suspension.
7. Press F at the cockpit. W/S throttle, A/D steer, LeftControl handbrake, G/H tire pressure, F to exit.

Expected feel on the hard slab: ~2.4 m/s^2 acceleration, ~31 km/h top speed, mild body roll in corners from the real CoM.

---

## 6. Verification plan

Engine paths verified on this machine: `C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe` and `...\Engine\Build\BatchFiles\Build.bat` both exist.

### (a) Terramechanics math tests, headless

Tests live in the runtime module under `#if WITH_DEV_AUTOMATION_TESTS`, named `Exoneer.Terramechanics.*` (`IMPLEMENT_SIMPLE_AUTOMATION_TEST`, ApplicationContextMask | ProductFilter). Run:

```
"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\Users\Mark\Documents\GitHub\Exoneer\Exoneer.uproject" -ExecCmds="Automation RunTests Exoneer.Terramechanics; Quit" -unattended -nopause -nosplash -nullrhi -log -abslog="C:\Users\Mark\Documents\GitHub\Exoneer\Saved\Logs\AutomationRun.log" -ReportExportPath="C:\Users\Mark\Documents\GitHub\Exoneer\Saved\Automation"
```

Pass criterion: the log contains no `Test Completed. Result={Fail` lines and the process exits 0. Required test vectors (computed from the section 4.1 formulas with the exact authored soil values, so a constants typo in either the C++ or the bootstrap fails loudly):

| Test | Inputs | Expected |
|---|---|---|
| Sinkage, dry sand | p = 80 kPa, b = 0.25 m | z = 0.0682 m, tol 1e-3 |
| Sinkage, clay | p = 80 kPa, b = 0.25 m | z = 0.0115 m, tol 1e-3 |
| Compaction resistance, dry sand | z = 0.0682 m, b = 0.25 m | R = 649 N, tol 2% |
| Slip term | s = 0.2, l = 0.1 m, K = 0.025 m | 0.3116, tol 1e-3 |
| Tractive effort, dry sand | A = 0.0257 m^2, W = 2058 N, c = 1.04 kPa, phi = 28 deg, slip term above | F = 349 N, tol 2% |
| Degenerate slip | s = 0 and s = 1e-6 | no NaN/div-by-zero, term monotonic to 0 |
| Monotonicity | z(p) rising in p; R(z) rising in z; F falling as s goes 0.25 to 1.0 | boolean checks |

### (b) Compile

```
"C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" ExoneerEditor Win64 Development -project="C:\Users\Mark\Documents\GitHub\Exoneer\Exoneer.uproject" -WaitMutex
```

This is the same target the PowerShell watcher rebuilds; every checkpoint below ends with this line green.

### (c) Bootstrap re-run, headless

```
"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\Users\Mark\Documents\GitHub\Exoneer\Exoneer.uproject" -run=pythonscript -script="C:/Users/Mark/Documents/GitHub/Exoneer/scripts/BootstrapPrototype.py" -stdout -unattended -nosplash
```

Pass criteria: `[Bootstrap] bootstrap complete` in output, the runtime-mapping assertion did not raise (now 20 bindings), and the log shows `spawned TestField_Sand` on first run after the map pass lands (and does not show it on the second run – the idempotency check). If level save misbehaves under the commandlet, fallback is running the same script once from the editor Python console; the script is unchanged either way.

### (d) Mark's playtest checklist, in order and in plain language

Solo, on the starter map, after building the section 5 rover:

1. Drive on the hard slab. It should pull away briskly, top out near 30 km/h, steer with the front wheels, and roll to a stop reasonably quickly when you release W – but coast, not brake. LeftControl should stop it firmly and hold it parked.
2. Drive east into the tan sand field at road pressure (220 kPa). Expect it to bite hard: sinkage climbs to ~17 cm on the HUD and the rover wallows to a stop within a couple of lengths. This is correct – road pressure on dry sand buries a tire.
3. Hold G and watch tire pressure fall. Around 90–100 kPa the sinkage readout should drop to ~7 cm and the rover starts crawling again. That is the CTIS flotation lesson.
4. Now floor W. Slip runs toward 1.0, the slip readout goes red, and forward progress collapses – you are digging in.
5. Ease off until slip sits in the marked 0.15–0.25 window. The rover should pull noticeably hardest right there.
6. Drive up the 10 degree ramp at low pressure with slip in the window – it should just climb. Try the 20 degree ramp – it should not, no matter the throttle. Back down in reverse.
7. Stop on the 10 degree ramp and pull the handbrake. The rover must hold, not creep.
8. Enter the curbed pit bay through the open side, bog it on purpose, then recover: pressure down, gentle reverse, out over your own tracks. Pump pressure back up (H) before returning to the slab – watch speed improve on hard ground at higher pressure.
9. Park the rover in the sand field, sunk, save, quit to menu, reload. The rover must come back complete – all four wheels present – at the same spot, with your tire pressure remembered.
10. Two-player listen server (PIE, 2 players, Play As Listen Server): the client passenger should see the driver's front wheels steer and all wheels spin at plausible rates, no rubber-banding at speed on the slab, and the sand field slowing both views identically. Swap roles and repeat briefly.
11. Battery check: note that a hard sand session visibly drains the batteries and the solar panels claw some back when parked in daylight.

Anything that deviates gets reported in plain language and lands as the next fix batch; the checkpoint structure below keeps every report against a rebuildable, playable editor state.

---

## 7. Rollout order and exclusions

Four checkpoints, each sized for one editor-close/rebuild cycle, each independently green (compile passes, bootstrap re-runs clean, game is playable at least at yesterday's level).

**Checkpoint 0 – soil in the world.** DefaultEngine.ini fixes (substep key, WheelProbe channel); `UExoneerSoilPhysicalMaterial` class (new file, no behavior); bootstrap: `ensure_block` seams, phys-material and MIC helpers, PM/MI assets, the unconditional ensure-actor map pass with all test-range actors, map comment fixes. Playable: identical gameplay, plus visible colored test fields and ramps to walk on. Verifies (b) then (c).

**Checkpoint 1 – rolling chassis.** Items, recipes, wheel block definitions, quick bar, starter items; the three IA assets and their character UPROPERTYs; `UWheelModule` with suspension + rigid-ground drive/steer/brake only (no Bekker yet); per-wheel visual components; module `Shutdown` hook; input decay/hold fix; placement allowance flag honored. Playable: the section 5 rover drives, steers, and parks on the hard slab. Bootstrap must run after this compile (new UPROPERTYs). Verifies (b), (c), playtest steps 1 and 7.

**Checkpoint 2 – soft soil.** Bekker-Wong path in the wheel module (sinkage, compaction resistance, slip-limited tractive effort), soil lookup via WheelProbe + PhysMaterial, damping tunables dropped to ~0 for wheeled constructs, and the full `Exoneer.Terramechanics` automation suite. Playable: playtest steps 2, 4, 5, 6 (at a fixed provisional pressure – CTIS is next). Verifies (a), (b), (c).

**Checkpoint 3 – CTIS, instruments, persistence.** G/H pressure pump end to end, drivetrain summary replication + HUD pilot panel, tire-pressure save/load field, save-path wheel-drop fix confirmed, listen-server pass. Playable: the entire checklist, steps 1–11.

**Explicitly out of this pass** (recorded so nobody "helpfully" adds them): gearboxes, drive shafts, and any drivetrain graph (every wheel is an in-hub motor); differentials and diff locking (all driven wheels receive the same torque command – noted as the follow-up that makes 6x6 interesting); tire wear, puncture, and debeading damage; deformable ruts persisting in the terrain and multi-pass sinkage (the ground never remembers); the wrist computer and the diegetic dashboard meshes (the HUD panel above is disposable scaffolding by design); the talent tree itself (the model keeps pressure, slip cap, and per-wheel load as live variables, which is all the hooks need); sandy loam and snow fields; per-biome gravity plumbing (gravity stays the world's -980, one defined source, documented).

### Critical Files for Implementation

- c:/Users/Mark/Documents/GitHub/Exoneer/scripts/BootstrapPrototype.py
- c:/Users/Mark/Documents/GitHub/Exoneer/Config/DefaultEngine.ini
- c:/Users/Mark/Documents/GitHub/Exoneer/Source/Exoneer/Data/VehicleBlockDefinitionDataAsset.h
- c:/Users/Mark/Documents/GitHub/Exoneer/Source/Exoneer/Player/ExoneerHUD.cpp
- c:/Users/Mark/Documents/GitHub/Exoneer/Source/Exoneer/Player/PlayerSurvivalCharacter.h