# Causal maintenance specification

Parent: [VISION.md](../VISION.md). Hard rules also live in [GAME-SCOPE.md](../GAME-SCOPE.md) §10.
This file is the implementable contract: two channels, measured causes, legal repair verbs.

---

## 1. Two channels (never mixed)

| Channel | What it is | Examples | Restore |
|---|---|---|---|
| **Immediate damage** (`Health`) | Discrete event, joules into a part | Impact, collapse, puncture, over-pressure burst, crash | Depends on failure: patch, replace, or the part is scrap |
| **Condition** | Continuous physical state of a working part | Tread depth, carcass temp, capacity fade, dust film, leak rate, seal deflection | Service, wipe, or **replace the part**. Not a heal-over-time |

`Health <= 0` on a part **removes or scraps it** (existing piece/block destruction paths). Condition at a terminal reading **makes the part stop being that part** (a wheel with 0 mm tread is scrap rubber, not a 0 HP tire you weld).

**Banned:** a single 0–1 “durability” bar; weld-to-heal on a Complete part; pawn health regen as the maintenance fantasy.

Existing violation to delete: `UBuildToolComponent` welding a Complete piece/block calls `RepairHealth`. That path dies when the first swappable class ships (wheels first).

---

## 2. Physical readings (what the visor/dash shows)

Readings are SI or published engineering units, never “72%”.

| Part class | Readings | Terminal |
|---|---|---|
| **Tire** | Tread depth (mm), carcass temperature (°C), inflation (kPa), (later) puncture open/closed | Tread 0 mm, or temp/pressure debead, or puncture + empty |
| **Hub motor** | Winding temp (°C), copper-loss watts, (later) insulation leak | Over-temp trip; capacity to make torque = 0 |
| **Battery** | Stored kJ, fade (effective capacity), temp | Fade to a floor; thermal trip |
| **Solar / radio face** | Dust optical depth (0–1 but *named* as opacity), watts or dBm actually delivered | Opacity → production ≈ 0 until wiped or panel swapped |
| **Seal** (suit, room, cabin) | Leak rate (L/s), deflection (mm) | Leak exceeds makeup O2 |
| **Structure** | SupportValue (1.0 integer budget), storm-exposed flag, fatigue from overload-hours | Collapse (existing solver) or scrap |

1.0 must ship the **tire, motor, battery, solar, seal, structure** rows at reduced order. Radio can share the solar “exposed face” model.

---

## 3. Causes (condition is spent by physics, not by time alone)

Time without load is almost free. Neglect of *exposure* is not.

| Cause | Spends | Measured from |
|---|---|---|
| **Use-wear** | Tire tread | `∫ k_tread · F_shear · v_slip dt` on contact - frictional WORK in the patch (Archard). A patch that does not slide does not wear, so firm ground is nearly free and a spinning wheel is expensive. |
| **Thermal** | Motor / battery | Copper loss `P_cu` and stall time (already in wheel power model) heat the winding. Cooling in air vs buried in mud differs. |
| **Overload** | Structure fatigue, tire carcass | Per-wheel `W` above static share; piece SupportValue near 0 for sustained time. |
| **Weather exposure** | Solar/radio opacity, structure pitting, seal | Storm intensity × exposed × (1 − shelter). Existing storm tick is the clock. |
| **Ecology fouling** | Tread / intakes / solar | Flora contact (no fauna). Light 1.0 term. |
| **Immediate event** | Health, or a boolean (punctured) | Impact, collapse debris, sharp rock (post-1.0 puncture). |

Prototype order: use-wear + thermal + weather exposure. Overload and fouling can be coefficients on those.

---

## 4. Legal repair verbs

The tool in the hand does not choose the outcome. The **failure mode** does.

| Verb | Legal on | Illegal on |
|---|---|---|
| **Weld** | Ghost → Complete (construction). Patching a *weldable crack* on metal structure (immediate damage, metal) | Tires, dust, seals, batteries, “health bars” |
| **Grind / cut** | Deconstruct, salvage | Healing |
| **Wipe / service** | Dust opacity, filters, fluids | Tread depth, faded capacity |
| **Patch** | Temporary seal or tire plug (if 1.0 even has puncture). Worse condition curve after | Restoring tread |
| **Replace** | Any part whose condition or damage is terminal for that part. Requires the fabricated spare. Unbolt time. | Inventing a spare |

Replace is the default 1.0 restore for tires, motors, solar panels, batteries.

Co-op: anyone with the spare and access can replace. No owner.

---

## 5. Diagnosis

No magic “repair kit” outline.

- **Visor:** emergency only (GAME-SCOPE §8) — suit kWh, time-to-empty, seal leak if critical.
- **Wrist:** logbook, net kW, condition of the *focused* part in physical units.
- **Dash:** per-wheel tread, temp, kPa; motor temp; battery kJ and fade. Needles bounce (stylized).
- **Look at the part:** dust film, glazed tread, sag, sparks — readable at 20 m (art contract).

If a reading is not on this list, the player cannot be required to know it for 1.0.

---

## 6. Replication and save

- Condition replicates with the part (piece actor or vehicle block record / side array). Deadband like wheel telemetry so it does not spam.
- Save: persist readings, not derived “%”. See ARCHITECTURE-V2 §16 and `FSavedVehicleBlock` / `FSavedBasePiece` extensions.
- Join-in-progress: late client must see the same tread mm, not a freshly spawned nominal tire.

---

## 7. 1.0 teaching cases (minimum mean)

1. Hard-driven rover on clay/sand: **must swap a wheel** before a long ridge haul, or traction collapses for a physical reason (tread or temp).
2. Ignored hut through a named storm: solar watts drop (dust) and/or exposed pieces lose condition; a weld beam cannot skip it.
3. Battery proximity-sip is replaced by an umbilical; suit power is maintenance of a machine, not a pickup.

Expand classes only after these three feel true.

---

## 8. Interaction with talents (later)

When talents exist: they unlock **readings and control** (stress overlay, CTIS low band, slip governor). They never reduce wear by a percentage. GAME-SCOPE §7 still wins.
