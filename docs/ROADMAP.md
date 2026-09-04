# Exoneer roadmap – Sandbox Road to Orbit

Dependency-ordered build, planned systems, verification matrix.
Product: [VISION.md](VISION.md). Specs: [design/sandbox.md](design/sandbox.md), [design/maintenance.md](design/maintenance.md), [design/projects.md](design/projects.md), [design/escape.md](design/escape.md).

## Status snapshot – 2026-09-03

| Phase | Status | Evidence |
|---|---|---|
| 0 Design contract | Complete, uncommitted | VISION, GAME-SCOPE §10, ARCHITECTURE-V2 §§16–18, four specs, this file |
| 1 Risk prototypes | P1 playtested (rounds 1-4). P3 and P4 code done, PIE verification pending. P2 blocked: span never sees rover mass | `WheelModule.cpp`, `VehicleConstruct.cpp`, `BaseStructure.cpp` |
| 2 Sandbox | Partial. Loop closes in creative mode; basin is a test range, not authored | `BootstrapPrototype.py`, `L_StarterPlanet` |
| 3 Causal decay | Tires and dust done. Motor temp written, not read. Battery, seal, structure fatigue missing | `ExoneerMaintenance.h` |
| 4 Project framework | Done. 8 criterion types, RPC, GameState replication, save | `ProjectSubsystem.cpp`, `ExoneerGameState.cpp` |
| 5 Projects | Long Watch runnable. Handshake cannot fail (no window criterion, DurationSols 0). Road to Orbit criteria partial | `BootstrapPrototype.py` project defs |
| 6 Validate | Not started | – |

### Open gaps, in order

1. Commit the working tree.
2. V-SPAN: deck piece reads vehicle contact load into the support solver.
3. V-HAND: add DishOnAzimuth and window criterion types, set DurationSols so the Handshake can fail, log the fail line.
4. Consume motor winding temperature (derate, trip, dash line).
5. Umbilical actor replaces the proximity sip.
6. Battery fade, seal leak, structure fatigue classes.
7. Delete dead heal declarations and make the no-weld-heal test weld a Complete wheel through BuildTool and assert Health and tread unchanged.
8. Author the basin (clay cut, sand bowl, grade, wash, ridge, two wrecks, flora).
9. Two-client JIP spot check when a second tester exists.

---

## Phase 0 – Design contract (complete 2026-09-03)

| Deliverable | File |
|---|---|
| Sandbox-first north star | `docs/VISION.md` |
| Causal maintenance hard rules | `docs/GAME-SCOPE.md` §10 |
| Condition, project, persistence seams | `docs/ARCHITECTURE-V2.md` §§16–18 |
| Four specs | `docs/design/{sandbox,maintenance,projects,escape}.md` |
| This matrix | `docs/ROADMAP.md` |
| ADRs (sandbox-first, no weld-heal, project subsystem) | `docs/adr/ADR-001` … `ADR-003` |

**Exit:** a new engineer can implement without inventing a campaign.

C++ for P1, P3, P4, condition (tires, dust), the replace verb, cargo mass, fuel, civil pieces via bootstrap, the visor machine and project panels, the proximity O2/power sip, the project subsystem and save exist uncommitted. P2 span load and the Handshake window do not. Re-run `scripts/BootstrapPrototype.py` in the editor so data assets exist, then PIE.

### Doc debt

- `GAME-SCOPE.md` §9: the "CTIS talent-gate conflict" paragraph is stale; no live valve exists to gate.
- `GAME-SCOPE.md` §7: the "Combat & hazard path" talent group conflicts with VISION's no-combat rule; needs renaming.
- `GAME-SCOPE.md` §9: gap map rows 5 and 8 say condition/part-swap and diegetic UI are not started.
- `ARCHITECTURE-V2.md` lines 3–5 and the §§16–18 headers say "not implemented".
- `GAME-SCOPE.md` §8: the wrist bullet omits the project list.
- `docs/design/wheels/*.md`: describe the removed live CTIS; carry no superseded marker.

---

## Phase 1 – Prototype major risks (parallel)

Do these as four thin verticals. Do not wait for art.

| Prototype | Code / content | Passes when | Status |
|---|---|---|---|
| **P1 Loaded rover** | Existing wheels + cargo mass on bed; firm / sand / clay pads | Road tires bog on sand; sand or snow family floats; clay punishes raw throttle; brake holds 10° firm | Playtested; sand and clay confirmed after slip-velocity fix |
| **P2 Bridge / shelter** | Floor/ramp/beam + a deck piece; support solver; storm tick | Loaded rover collapses an undersupported span; sheltered solar weathers a storm better than exposed | Pieces exist; support solver ignores vehicle mass (blocker) |
| **P3 Tire wear** | Condition on wheel records; visor/dash mm + °C; replace consumes a tire item; landing-impact damage; replicate; save | Hard drive spends tread; replace restores; late joiner sees the worn tire; load keeps mm | Code done; test coverage weak |
| **P4 Ascent craft** | Fuel module + thrusters + gyro on a grid stack; pad hold-down optional | Off-axis thrust rotates; empty tank kills thrust; flip is possible; TWR readable | Code done; PIE go/no-go pending |

**Exit:** go / no-go on Road to Orbit *in 1.0* from P4. P1–P3 are not optional.

Weld-to-heal is a no-op (done). Still to do: delete the dead RepairHealth, RepairBlockAt and RepairHealthPerWeldPoint declarations and strengthen the automation test.

### P4 sim bar (measurable)

Five checks. All must pass in PIE and be recorded:

a. With the gyro removed, an off-axis thruster produces a measurable rotation rate.
b. An empty tank drops thrust to zero within one physics tick.
c. HUD TWR matches thrust divided by mass times local g within 5 percent.
d. With no gyro block fitted there is no attitude hold.
e. At least one honest failure is reproducible on demand: flip from off-axis thrust, brown-out from power draw, or pad strike.

Pass on all five: Road to Orbit stays in 1.0 scope. Any miss: slip intact to post-1.0, no scripted launch.

---

## Phase 2 – Unrestricted sandbox

Depends on P1–P3.

- Finish suspension, steer, brake, handbrake, wheel-family swaps, cargo, recovery, soil feedback (close remaining wheel-pass gaps)
- Staged construction, foundations, support, collapse, roads/bridges, salvage
- Close mine → fabricate → operate → maintain (machine UI, O2 refill, umbilical)
- Author the basin: soils, clay cut, sand bowl, grade, wash, ridge, two wrecks, flora
- Solo + 1–4 listen-server saves from the outset (extend save records per §18)
- Flip exoneer.Creative off and test the survival economy
- Basin today is a test range (sand and clay fields, 10 and 20 degree ramps, curb bay, garage pad); the authored basin is new work

**Exit:** [sandbox.md](design/sandbox.md) completeness test, including a blind onboarding run.

---

## Phase 3 – Causal decay (full 1.0 set)

Depends on P3.

Expand condition from tires to motors, batteries, solar/radio faces, seals, structures per [maintenance.md](design/maintenance.md). Diagnosis in physical units. Legal verbs only.

Done: tire tread, dust opacity. Written not consumed: motor winding temperature. Missing: battery fade, seal leak, structure fatigue. Landing-impact damage exists as an immediate-damage channel.

**Exit:** teaching cases (wheel swap before ridge; dusty solar after storm; no beam).

---

## Phase 4 – Optional-project framework

Depends on Phase 2 persistence.

`UProjectDefinitionDataAsset`, `UProjectSubsystem`, player-owned accept/abandon RPCs, criterion evaluator, wrist list, save/JIP.

Status: done in the working tree (UProjectDefinitionDataAsset, UProjectSubsystem, Server_ToggleProject, AExoneerGameState, save).

**Exit:** a project can succeed and fail on physical readings; ignoring projects does not brick the save.

---

## Phase 5 – Projects in dependency order

| Order | Project | Depends on | Status |
|---|---|---|---|
| 5a | The Long Watch | Phase 3 + 4 + storms | Runnable |
| 5b | The Handshake | 5a not required; needs dish piece, haul, relays, window clock; azimuth and window criterion types, DurationSols > 0 | Cannot fail yet |
| 5c | Road to Orbit | P4 go; Phase 2 roads/haul; Handshake data optional | Criteria partial (PadPowered, VehicleFuel, VehicleTwr exist) |

**Exit:** each project's fail log is an engineering sentence. Handshake does not end the game.

---

## Phase 6 – Validate before expanding

| Test | Pass |
|---|---|
| Blind sandbox onboarding | No accepted mission; completeness test |
| Seven-sol soak | Long Watch can be passed and failed |
| Handshake fail / retry | Brown-out and yaw both logged; next window works |
| Heavy-haul to pad | Loaded rover on the authored route; wear + maybe a bridge |
| Ascent (if P4 go) | Static-fire fail and a clean reduced-order ascent |
| 1–4 replication | Two clients weld one ghost; late join; save/load |

After Phase 6: EOS, second world, talents, FEM, fluids, puncture, ruts – not before.

---

## Planned systems vs 1.0

| System | 1.0 | Later |
|---|---|---|
| Socket bases + support budget | Yes | Vector load, sag |
| Grid vehicles + wheels | Yes, four wheel families | Live pressure valve (talent), gearboxes, ruts, puncture meshes |
| Physical gyro block | Yes | – |
| Fuel tank + thrust starvation | Yes | Fluids, cooling loops |
| Causal condition | Yes (reduced-order) | Full catalog |
| Machine craft loop | Yes | Heat/cooling loops |
| Umbilical | Yes | Full cable physics |
| Relays / wrist | Yes (simple LOS) | Full telemetry sim |
| Long Watch / Handshake | Yes (optional) | More projects |
| Ascent | Gated | Full orbital |
| Multi-planet | Sky only | Transfer craft |
| EOS / talents / PCG | No | Yes |

---

## Verification matrix (contracts)

| ID | Contract | Evidence | Status 2026-09-03 |
|---|---|---|---|
| V-SANDBOX | Play without projects | Blind completeness test | Runnable |
| V-SOIL | Three soils teach | P1 playtest notes + terramechanics unit tests (exist) | Yes (terramechanics suites green, playtested) |
| V-SPAN | Bridge carries or collapses | P2: SupportValue ≤ 0 under loaded rover | Not wired |
| V-TREAD | Wear is causal | P3: `∫ |s| W dt` moves mm; save/JIP | Yes |
| V-NOWELDHEAL | No beam | Automation: weld on Complete wheel does not restore tread or Health | Behaviour yes, test weak |
| V-CRAFT | Economy closed | Recipe in → item out without debug grant | Pre-existing, not re-verified |
| V-SAVE | Persistence | Hut, rover, condition, project state round-trip | Yes |
| V-JIP | Late join | Second client sees worn tire and active project | Untested with two clients |
| V-WATCH | Seven sols | Criteria hold or fail physically | Runnable |
| V-HAND | Dish window | Azimuth + power + hop; fail/retry | Not implemented |
| V-ORBIT | Gate | P4 prototype or explicit slip to post-1.0 | Satisfiable pending PIE sim bar |

---

## Critical implementation files (from current layer)

- `Source/Exoneer/Vehicles/VehicleConstruct.*`
- `Source/Exoneer/Vehicles/WheelSimCallback.*`
- `Source/Exoneer/Vehicles/WheelModule.cpp`
- `Source/Exoneer/Vehicles/VehicleWheelSpec.h`
- `Source/Exoneer/Building/BaseStructure.*`
- `Source/Exoneer/Components/ConstructionComponent.*`
- `Source/Exoneer/Components/BuildToolComponent.*` (heal path is a no-op; dead declarations to delete)
- `Source/Exoneer/World/PlanetEnvironmentManager.*`
- `Source/Exoneer/World/ProjectSubsystem.*`
- `Source/Exoneer/World/ExoneerGameState.*`
- `Source/Exoneer/Data/ProjectDefinitionDataAsset.h`
- `Source/Exoneer/Maintenance/ExoneerMaintenance.h`
- `Source/Exoneer/Tests/ExoneerMaintenanceTests.cpp`
- `Source/Exoneer/Save/ExoneerSaveGame.h`
- `scripts/BootstrapPrototype.py`

---

## How we iterate

- Owner playtests and reports in plain language; fixes land in C++.
- Owner closes the editor; `Build.bat` runs (a watcher waits for UnrealEditor to exit and kills LiveCodingConsole); owner waits for "build green" before reopening, because reopening early rolls back to the old DLL.
- Bootstrap is idempotent; re-run after data changes.

Headless tests run via UnrealEditor-Cmd. Kill any zombie UnrealEditor-Cmd holding port 8001 first.

```
UnrealEditor-Cmd.exe <path>/Exoneer.uproject -ExecCmds="Automation RunTests Exoneer.Terramechanics; Quit" -TestExit="Automation Test Queue Empty" -unattended -nullrhi -nosplash -nop4 -log
UnrealEditor-Cmd.exe <path>/Exoneer.uproject -ExecCmds="Automation RunTests Exoneer.Maintenance; Quit" -TestExit="Automation Test Queue Empty" -unattended -nullrhi -nosplash -nop4 -log
```

Results land in `Saved/Logs/Exoneer.log`.
