# Exoneer: master game scope architecture

Version: 2.0 – permanent plan and strict scope boundary.
Engine: Unreal Engine 5.8+ (Chaos Physics, Nanite, Lumen, Substrate).
Genre: first-person hyper-realistic engineering and logistics survival sandbox.

This document is the boundary for all design and implementation work. Features
outside these eight modules are out of scope. Features inside them must follow
the realism rules stated here – in particular section 7's ban on abstract stat
bonuses and section 8's ban on screen-space HUD bars. The current C++
implementation layer is documented in [ARCHITECTURE-V2.md](ARCHITECTURE-V2.md);
section 9 below maps it against this scope.

---

## 1. Player, suit and life support

- No biological micromanagement: no hunger or thirst meters. Tension comes
  from engineering failures, environmental hazards, and suit resources.
- **Suit power** is the engineer's lifeline. Power drains continuously to
  maintain internal atmospheric pressure, O2 scrubbing, and thermal
  regulation. Heavy power tools accelerate the drain. Recharging happens via
  physical umbilical cables at base stations or inside vehicle cockpits.
- **Haptic movement**: the player has true physical mass and momentum.
  Movement speed, braking distance, and turning inertia scale realistically
  with surface friction coefficients (thick mud, loose sand, sheer ice) and
  local planetary gravity.
- **The fabricator tool**: a heavy, multi-mode handheld industrial tool with a
  high-heat plasma cutter for salvage/deconstruction and a precision
  arc-welder for structural assembly.

## 2. The structural physics building system

- **Strict civil engineering**: zero floating voxels or blocks. Construction
  is bound by structural load paths and material strength limits.
- **Material fatigue and stress**: every structural element (beam, pillar,
  panel) has exact mass, material density, and maximum yield strength.
- **Live stress calculation**: the engine computes real-time vector loads.
  Overburdened structures visibly sag, warp, twist, and throw sparks
  (Substrate shaders and vertex deformation) before failing completely
  through Chaos Physics destruction.
- **Grid and free-placement hybrid**: large foundational components (pillars,
  walls, floors) snap to a global grid; internal utilities, decorations, and
  small machinery place freely with precise translation and rotation.
- **Utility plumbing and wiring**: bases need physical infrastructure.
  High-voltage copper wiring grids carry electrical current; high-pressure
  piping loops move volatile fuels, water, and gases between storage cells,
  generators, and manufacturing lines.

## 3. Atmospheric and fluid physics

- **Pressurization**: enclosed structures need active life support (O2 tanks,
  scrubbers, compressors) to become breathable.
- **Airlock logic**: entering or leaving a pressurized base requires
  functional multi-door airlock sequences to avoid atmospheric loss.
- **Decompression events**: structural breaches (weather debris, collapse)
  cause explosive or gradual decompression. The pressure differential
  generates kinetic force that physically throws the player and loose
  physics objects toward the breach.
- **Liquid dynamics**: fuels and water must be piped correctly. Pipe networks
  simulate head loss (friction inside pipes), valve pressure limits, and
  gravity-assisted flow.

## 4. Modular physics-driven vehicles

- **Chassis block assembly**: vehicles are built piece by piece. Players
  physically mount independent double-wishbone suspensions, steering racks,
  gearboxes, internal combustion or electric hub motors, and drive shafts.
- **Real-time center of mass**: heavy components, cargo bays, and auxiliary
  fuel tanks dynamically update the vehicle's CoM. Top-heavy or asymmetric
  rovers lean, lose tire traction, or roll over when cornering or traversing
  steep topography.
- **Tire deflection and terrain interaction**: tires physically compress and
  bulge under cargo weight. Vehicles realistically bog down and sink into
  soft substrate, requiring AWD drivetrains, locking differentials, and
  adjustable tire pressures.

### 4.1 Terramechanics model (Bekker-Wong)

Talent effects and driving behavior in soft terrain modify the actual
physical variables of these formulas – never abstract percentages.

**Sinkage** – how deep (m) a tire sinks into soft ground:

$$z = \left(\frac{p}{\frac{k_c}{b} + k_\phi}\right)^{\frac{1}{n}}$$

where `z` = sinkage depth (m), `p` = ground pressure (kPa), `b` = tire width
(m), and `k_c`, `k_phi`, `n` = the substrate's material constants (cohesion,
friction, deformation exponent).

**Gross tractive effort** – the maximum forward force the wheel can push with,
limited by the substrate's shear strength:

$$F = (A \cdot c + W \cdot \tan\phi)\cdot\left(1 - \frac{K}{s \cdot l}\left(1 - e^{-\frac{s \cdot l}{K}}\right)\right)$$

where `A` = tire contact area (m²), `W` = vertical wheel load (N), `c` =
soil cohesion, `phi` = internal friction angle, `s` = slip ratio, `l` =
contact patch length (m), `K` = shear deformation parameter (m).

**Motion resistance** – the compaction drag the motor must overcome because
the tire keeps climbing the soil wave it makes:

$$R = b\left(\frac{k_c}{b} + k_\phi\right)\frac{z^{\,n+1}}{n+1}$$

Design consequences: raw throttle in mud drives the slip ratio toward 1.0 and
collapses tractive effort (you dig in); heavy cargo raises `W`, which raises
`z`, which explodes `R`. Skilled play means managing `p`, `s`, and load
distribution – which is exactly what the Driver talents calibrate (section 7).

## 5. Industrial operations and maintenance

- **Resource extraction**: drilling raw geological deposits (iron, copper,
  titanium, silicon) with manual heavy thermal drills or vehicle-mounted
  rotary boring rigs.
- **Thermodynamic refining**: blast furnaces and chemical processors smelt
  raw ore into pure ingots. The loop generates massive heat footprints and
  requires physical cooling loops and exhaust vents.
- **Precision fabrication**: automated CNC assembly stations and heavy 3D
  printers combine refined metals into high-tolerance mechanical and
  electrical components (pistons, microprocessors, hydraulic lines).
- **Mechanical wear and repair**: machinery and vehicle components suffer
  structural attrition. Gears strip under high-torque abuse; tires puncture
  on sharp rock. Repairs are never a magical healing beam: the player
  physically unbolts the broken sub-component and bolts on a manufactured
  replacement part.

## 6. Environmental logistics and weather

- **Volumetric meteorology**: storm fronts (abrasive sandstorms, superheated
  windstorms, corrosive rain) are true physical, volumetric entities. They
  obliterate visibility and apply aerodynamic drag forces that can rip
  unanchored structures apart.
- **Environmental wear and corrosion**: severe weather slowly degrades
  exposed equipment, solar arrays, and unprotected vehicles – forcing
  enclosed hangars, protective shielding, or routine maintenance.
- **Signal and telemetry logistics**: no automatic global map. The player
  builds radio towers and satellite uplinks to chart terrain. Driving behind
  mountains or into caverns causes a telemetry blackout, severing the wrist
  computer's link to the base grid until a line-of-sight relay is deployed.
- World generated with UE Procedural Content Generation (PCG).

## 7. The engineer progression (talents)

**Realism rule (hard constraint): talents never grant abstract, magical RPG
bonuses. "+10% damage" and its relatives are prohibited.** Talents unlock
advanced diagnostic software, manual override controls, and finer mechanical
calibration – operational precision, HUD instrumentation, physical control.

### Mechanical path
- *Structural Integrity*: unlocks HUD overlays with real-time vector
  stress/weight loads on buildings; allows joint reinforcement without added
  physical bulk.
- *Industrial Tuning*: raises the thermodynamic efficiency of smelters and
  assembly lines, reducing power draw or heat dissipation.

### Driver path
- *Heavy Off-Road*:
  - **Level 1 – Pressure Calibration**: unlocks an automatic tire-pressure
    valve (CTIS) in the cockpit. Lowering `p` (for example by 40%) shrinks
    the numerator of the sinkage formula, so `z` drops sharply and the tire
    floats on top of the mud (visualized through deformable meshes).
  - **Level 2 – Shear Control**: introduces micro-throttle scaling in the
    input code that automatically holds the slip ratio inside the optimal
    mud window (`s = 0.15..0.25`), maximizing tractive effort `F` without
    stalling the engine. Without it, raw throttle sends `s` toward 1.0 and
    traction collapses.
- *Logistics Carrier*:
  - **Level 1 – Dynamic Dampening**: drives the active suspension to spread
    the load `W` evenly across all axles (6x6, 8x8), minimizing the maximum
    per-axle sinkage `z` and thereby holding total motion resistance `R` at
    its theoretical minimum under heavy cargo.

### Pilot path
- *Avionics & Lift*: optimizes thruster fuel consumption and speeds recovery
  from aerodynamic stalls in heavy windstorms.
- *Vector Control*: unlocks synthetic-vision flight instrumentation for
  precise, instrument-only landings in zero-visibility storms.

### Combat & hazard path
- *Environmental Shielding*: calibrates suit seals against thermal spikes,
  corrosive rain, and impact from structural debris.
- *Kinetic Tools*: improves stability and recoil control of heavy plasma
  cutters and mining drills, making them usable as improvised defense
  against environmental kinetic threats.

### Integration sketch (Chaos Physics tick)

```cpp
// Terrain data from PCG/Landscape surface material
float MudCohesion  = SurfaceMaterial.Cohesion;
float MudFriction  = SurfaceMaterial.FrictionAngle;

// Tire pressure (player CTIS input)
float TirePressure = Vehicle.GetTirePressure();

// TALENT: Heavy Off-Road Lvl 1 permits extreme decompression without debeading
if (Engineer.HasTalent("Heavy_Off_Road_Lvl1") && Vehicle.CTIS_Active)
{
    TirePressure *= 0.60f;
}

float Sinkage = CalculateBekkerSinkage(TirePressure, MudCohesion, MudFriction);

// TALENT: Heavy Off-Road Lvl 2 caps torque to hold the optimal slip window
if (Engineer.HasTalent("Heavy_Off_Road_Lvl2") && Sinkage > 0.05f)
{
    Vehicle.CapEngineTorqueToMaintainOptimalSlip(0.20f);
}

Vehicle.ApplyMotionResistanceForce(Sinkage);
```

## 8. Diegetic and diagnostic UI

- **Zero screen-space 2D HUD**: traditional arcade bars (HP, mana, generic
  energy) are strictly banned. All data lives in world space.
- **The helmet visor**: fighter-jet style instrumentation drawn as monochrome
  vector lines on the glass. Emergency data only: suit power (kWh plus
  estimated time to empty), jetpack fuel (liters of nitrogen), suit
  integrity (atmospheric seal %).
- **Wrist diagnostic computer**: accessed by looking at the left forearm. A
  rugged industrial screen for wireframe structural scans, power grid
  net-flow graphs (+/- kW), and technical logbooks.
- **Physical dashboards**: vehicles and industrial stations use built-in
  analog and digital cockpits. Fuel gauges are mechanical needles that
  bounce with terrain; power draw shows on analog ammeters; component
  integrity appears on monochrome CRT/LCD schematic readouts in the dash.

---

## 9. Gap map: current implementation vs this scope

The construction layer v2 ([ARCHITECTURE-V2.md](ARCHITECTURE-V2.md)) is the
replicated foundation this scope builds on. Status per module:

| Scope module | Current layer | Gap to close |
|---|---|---|
| 1. Suit & life support | Suit power/O2/temperature replicated; **Nutrition stat still exists** | Remove Nutrition (scope bans hunger); umbilical recharge; haptic mass/friction movement; fabricator tool modes |
| 2. Structural building | Socket snapping, support-budget solver, ghost-then-weld, tiers, storm damage | Replace budget solver with vector load solver + sag/warp visuals; free-placement layer; wiring/piping networks |
| 3. Atmosphere & fluids | Oxygen generator machine only | Pressure volumes, airlocks, decompression forces, pipe flow simulation |
| 4. Vehicles | Unified 25 cm grid, welded rigid body, emergent CoM/mass, thrusters, split detection; wheels with Bekker-Wong terramechanics per fixed physics substep (dual rigid/pneumatic regime, slip sinkage, Janosi-Hanamoto combined shear, compaction + bulldozing resistance, hub motors with copper loss, CTIS tire pressure, Ackermann steering) | Gearboxes/drive shafts/differentials as physical parts, tire wear/puncture, deformable tire meshes, persistent ruts, multi-pass sinkage |
| 5. Industry & maintenance | Refine/fabricate machines with power-scaled crafting | Heat footprints/cooling, mechanical wear, part-swap repair (**current weld-to-repair is a placeholder that violates the no-healing-beam rule and must be replaced by part replacement**) |
| 6. Environment & weather | Replicated day/night + storms with per-tier structure damage | Volumetric storm fronts with drag forces, corrosion, telemetry/signal system, PCG world |
| 7. Talents | Not started | Talent tree data model + the physical-calibration hooks per path (no stat boosts) |
| 8. Diegetic UI | Not started (HUD events exposed as delegates) | Visor/wrist/dashboard instrument stack; delete any screen-space bars as this lands |

Known conflicts to resolve as their systems come up (flagged, not silently
changed): the weld-to-repair placeholder, and the CTIS tire-pressure valve
being available to everyone with a full 20-300 kPa range - section 7 gates
extreme decompression behind Heavy Off-Road Level 1, so when talents land the
ungated minimum must rise to a debead floor (~60 kPa) with the talent
unlocking the low band. (The `Nutrition` stat was removed as scoped.)
