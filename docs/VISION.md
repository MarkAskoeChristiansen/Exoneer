# Exoneer – product vision (Sandbox Road to Orbit)

Status: working north star. This file answers *what game we are shipping*.

Snapshot: 2026-09-03. Design contract complete. Risk-prototype code (P1-P4), condition, projects and save seams exist in the working tree, uncommitted. Live status: ROADMAP.md status table.

| Doc | Authority |
|---|---|
| **This file** | What the game is: sandbox-first, optional projects, Road to Orbit as the unique endgoal |
| [GAME-SCOPE.md](GAME-SCOPE.md) | How systems must behave (eight modules, no stat boosts, no HUD bars, **causal maintenance**) |
| [ARCHITECTURE-V2.md](ARCHITECTURE-V2.md) | Current C++ construction layer + the v3 seams for condition, projects, persistence |
| [design/sandbox.md](design/sandbox.md) | Unrestricted play, economy, basin, saves |
| [design/maintenance.md](design/maintenance.md) | Damage vs condition, diagnosis, legal repair verbs |
| [design/projects.md](design/projects.md) | Optional project framework (Long Watch, Handshake) |
| [design/escape.md](design/escape.md) | Road to Orbit: pad, stack, ascent bounds |
| [design/wheels/](design/wheels/) | Bekker-Wong wheel math, engine plan, critique. The live tire-pressure control described there was removed in fd26684; the math is still current. |
| [ROADMAP.md](ROADMAP.md) | Dependency-ordered build and verification matrix |

When a feature disagrees with *what the game is*, this file wins. When it disagrees with *how a system must behave*, GAME-SCOPE wins.

---

## 0. The game in one breath

**Exoneer is a sandbox first.** A 1–4 player first-person engineering survival game whose difficulty is real work on a living basin. Players can mine, refine, fabricate, construct, drive, inspect, repair, and expand **forever without accepting a project**. Later projects only test systems they already use.

The look is chunky, colorful, cartoonish sci-fi – so a co-op partner can read a failure from across the valley. The job is not cartoon.

Four verbs, all physically honest:

| Verb | What "realistic" means |
|---|---|
| **Construction** | Ghost frame, haul materials, weld in stages. Nothing finishes because you clicked Place. |
| **Building** | Civil engineering. Load paths, sockets, no floating kits, weather loads. Roads, bridges, shelters are structures. |
| **Driving** | Mass, CoM, wheel family vs soil, slip, sinkage, ballast, braking, wear. The rover you welded is the rover you drive. |
| **Maintenance** | Immediate damage and long-term **condition** are separate. You diagnose physical readings and apply the repair that matches the failure – never a healing beam. |

You do not eat. You do not level up. You do not get a free map. Core tools are never mission-locked.

The unique endgoal – **optional**, never an ending forced on the save – is **Road to Orbit**: the rocket is only the last vehicle in a logistics chain the player physically created. The colony *is* the spacecraft-support system.

Tagline: **It looks like a toy. It fails like a machine. The colony launches the rocket.**

---

## 1. Unique thesis

Not "cute Space Engineers." Not "Icarus with trucks." Not a campaign with a sandbox attached.

**The empty cell:** honest construction + honest dirt + honest wear, in a place you can live in indefinitely, with an optional rocket whose difficulty is *the colony you already built*.

| Game | Constructs | Drives dirt | Maintains by cause | Sandbox without quests | Rocket is a logistics exam |
|---|---|---|---|---|---|
| Space Engineers | Yes | Weak in atmo | HP | Yes | Creative-mode stack on a quarry |
| Icarus | Sockets | No | Weather timer | Session-gated | No |
| SnowRunner | No | Yes | Parts you didn't build | Yes | No |
| Kerbal | Hangar | No | Staging, not wear | Sandbox | Yes – no colony behind it |
| Satisfactory | Factory | No | No | Yes | No |
| Empyrion | Mashup | Mashup | Mashup | Thin | Menu |
| **Exoneer** | **Yes** | **Yes** | **Yes** | **The game** | **The optional final vehicle** |

This draws on SnowRunner's terrain, Space Engineers' construction, and KSP's honest failures. The combination is distinct: **escape is a logistics problem whose factory, roads, and spares you already operate.**

Thesis, hard:

1. **Sandbox autonomy.** A player who never opens the wrist-computer project list is playing the whole game.
2. **If it would not work, it does not work.** No +10% talents. Calibration only.
3. **Causal maintenance is the long loop.** Use, overload, weather, and time spend *measurable* condition. Repair verbs match failure modes.
4. **Stylized look, realistic job.** Color and silhouette are diagnostics. Grey SE quarries are banned. Cartoon physics is banned.
5. **Two grammars of making.** Bases = civil (sockets, load paths). Vehicles = mechanical (25 cm grid, mass, CoM). The ascent vehicle is the second grammar under flight loads.
6. **Ghosts are invitations.** Any player can finish any ghost.
7. **Projects evaluate physics, not tokens.** No "bring 10 iron" quest. The Long Watch, the Handshake, and Road to Orbit query the world.
8. **Road to Orbit is optional and may slip a release.** It must not become a cutscene. If the flight prototype fails the sim bar, 1.0 ships the sandbox without it.
9. **Progression is equipment, not sliders.** Wheel families, motor ratings and later talents gate what a crew can do. A control the player must babysit every drive (a live tire-pressure valve) is out. Early difficulty comes from power scarcity, not from menus.

Nameable Steam sentence:

> Weld it true, drive it honest, watch it wear, keep it alive. A colorful co-op engineering sandbox – and if you want off this rock, the rocket is the last machine in a chain you built with your hands.

---

## 2. Sandbox autonomy

The sandbox is complete when a crew can:

- Mine, refine, fabricate, and stock spares
- Ghost and weld a hut that stands, a road or bridge that carries a loaded rover, a shelter that takes a storm
- Drive a construct they welded across **firm ground, sand, and clay**, including cargo mass, the right wheel family for the soil, braking, recovery
- Inspect physical readings (tread, temp, leak, capacity, deflection, kW)
- Repair with the legal verb for the failure
- Sleep through nights and storms because life support, power, and seals were maintained
- Save and reload; a second player can join and weld the same ghost

Nothing in that list requires a project. Spec: [design/sandbox.md](design/sandbox.md).

**Anti-patterns (never):**

- Mission-gated pieces, recipes, or tools
- An empty quarry "until you start the story"
- A fail-state that wipes the colony because you ignored a project
- XP, skill trees as unlock walls, hunger

---

## 3. Optional projects

Accepted on the wrist computer. Ignored, abandoned, or retried freely. Shared, server-authoritative. Success and failure are **physical**. Spec: [design/projects.md](design/projects.md).

| Project | What it tests | What it is not |
|---|---|---|
| *(none)* | The sandbox | – |
| **The Long Watch** | Seven sols of self-sufficiency, including one severe storm: life support, reserves, comms, locally produced spares | A timer quest with a loot chest |
| **The Handshake** | Build and haul a dish; establish orbital contact | Mandatory, an ending, or a map unlock wall. It yields **escape-specific orbital data** |
| **Road to Orbit** | Survey, roads, heavy haul, pad, stack, certify, static-fire, ascent | A cutscene. The rocket is a vehicle |

After a successful ascent: depart, return by capsule to the **same persistent colony**, or leave crewmates operating it. The save does not end.

---

## 4. Unique endgoal: Road to Orbit

To leave, players must actually:

1. Survey a launch site by slope, soil, altitude, weather, and access.
2. Build roads, bridges, winch points, depots, and storm shelters.
3. Design heavy haulers around traction, axle load, wheel family, braking, and wear.
4. Manufacture and transport every launch module.
5. Construct and maintain pad, tower, power, cooling, and fuel.
6. Assemble and certify a modular ascent vehicle (grid-built: mass, off-axis thrust, fuel, control authority).
7. Survive weather delays, static-fire failures, and component degradation.
8. Pilot the ascent; co-op partners may run ground support.

The Handshake is useful here (ephemerides, windows) and **not required to keep playing**. Spec: [design/escape.md](design/escape.md).

**1.0 gate:** a bounded ascent-flight prototype must meet the simulation standard (mass, thrust, fuel, control, failure). If it does not, Road to Orbit moves intact to a later release. Never a scripted launch.

---

## 5. The basin (anti-Space-Engineers, 1.0 geography)

1.0 is **one detailed authored basin**, not a galaxy and not a grey sphere.

- Several meaningful soil families on the same map (at least firm, dry sand, clay)
- Logistics *routes*: grades, cuts, a crossing that wants a bridge, a ridge that wants a road
- Flora, wind, color, wrecks of prior crews (history, salvage). **No fauna, no combat, no hunt**
- Weather that loads structures and spends condition
- Sister bodies may hang in the sky as promise; they are not 1.0 destinations

"Not deserted" means the *place* is alive (ecology as flora/weather/history), not that it is an NPC game.

Post-1.0 worlds remain legal under GAME-SCOPE. They are not the 1.0 identity.

---

## 6. What 1.0 is (and is not)

### 1.0 ships

- Unrestricted sandbox on one authored basin (soils, routes, wrecks, flora)
- Salvage-tier sockets: foundation, wall, floor, ramp, roof, beam, door; roads/bridges/shelters as pieces
- Ghost-then-weld; mine → refine → fabricate → operate → maintain
- Machines with a usable enqueue/output loop (no quest required)
- Wheeled constructs: suspension, Ackermann steer, service brake, handbrake, four terrain wheel families (road, sand, mud, snow; steer and drive variants), cargo mass, recovery, soil feedback
- Physical gyro block: attitude authority only with a gyro fitted; saturation and power draw are real
- Fuel tank module: thrusters starve on an empty tank (the escape seam, already in)
- Causal maintenance v1: immediate damage ≠ condition; tires, motors, batteries, solar/radio exposure, seals, structures; diagnosis in physical units; **weld-to-heal is a no-op; passive health regen is zero**
- Physical umbilical for suit power / O2
- Visor (emergency) + wrist computer (projects, logs, relay-known terrain)
- Storms with drag, visibility, weather-spend
- 1–4 listen-server, save/load, join-in-progress
- Optional: Long Watch, Handshake (orbital data)
- Road to Orbit **only if** the flight prototype passes the sim bar

### 1.0 cuts (legal later)

| Cut | Why |
|---|---|
| Playable second planet / transfer craft | One basin must be deep |
| Vector-load FEM, full CFD/FEA, full orbital mechanics | Reduced-order SI models |
| Gearboxes / drive shafts as separate parts | In-hub motors are enough truth for 1.0 dirt |
| Persistent ruts, deformable tire meshes | Live sinkage + tread condition first |
| Talent tree | Calibration after the sandbox is fun |
| EOS | Listen-server first |
| PCG planet factory, Alloy/Composite catalogs | Place before planet-factory |
| Fauna, guns, hunger, factory-belt empire | Anti-fantasy / anti-Satisfactory |
| Towns / NPC factions | Dead voices and wrecks are enough company |
| Live tire-pressure valve (CTIS) | Removed as a control in fd26684. Pressure is a fixed wheel-family property and a reading. A Driver talent may add a valve later |

### Never

- Abstract stat-boost talents
- Screen-space HP/mana bars as identity UI
- Generic durability % or a healing beam
- Mission-locked core tools
- Combat / hunting progression
- A deserted quarry planet
- Road to Orbit as a cutscene
- An infinite procedural galaxy as 1.0
- A per-drive slider the player must babysit

---

## 7. Build order (summary)

Full matrix: [ROADMAP.md](ROADMAP.md). Note: step N here is ROADMAP Phase N-1.

1. **Design contract** (complete 2026-09-03): VISION, GAME-SCOPE causal maintenance, ARCHITECTURE seams, four specs, roadmap.
2. **Prototype major risks in parallel:** loaded rover on firm/sand/clay; bridge/shelter under load and wind; tire wear → diagnose → replace → replicate → persist; grid ascent craft (mass, off-axis thrust, fuel, control).
3. **Unrestricted sandbox:** finish drive, construction, economy, one deep basin, 1–4 saves.
4. **Causal decay** as the long loop (not a meter).
5. **Optional-project framework** (server state, physical criteria, diegetic telemetry).
6. **Missions in dependency order:** Long Watch → Handshake → Road to Orbit (gated).
7. **Validate before expanding:** blind sandbox onboarding; seven-sol soak; Handshake fail/retry; heavy-haul-to-rendezvous; 1–4 replication, late join, save/load.

---

## 8. Key decisions

| Decision | Choice | Why |
|---|---|---|
| Genre posture | Sandbox first | Projects that invent systems are a different (worse) game |
| Unique endgoal | Road to Orbit as optional logistics exam | Colony becomes the spacecraft-support system |
| Handshake | Optional; yields orbital data; not an ending | Useful, not a gate on play |
| Maintenance | Causal: damage ≠ condition; repair matches failure | GAME-SCOPE already banned the healing beam |
| 1.0 geography | One authored basin, several soils, logistics routes | Depth over planet count |
| Life | Flora, weather, wrecks; **no fauna** | Inhabited ≠ hunt; user 1.0 boundary |
| Combat / hunger / belts | Out | Would steal the antagonist from dirt and wear |
| Escape in 1.0 | Only if flight prototype meets the sim bar | Never a cutscene |
| Look | Chunky colorful readable | Diagnostics, not a fake job |
| Co-op | 1–4 listen-server in 1.0 | Persistence and late join are the social feature |
| Tire pressure | Not a control. Four wheel families; a talent may add a valve later | Owner disliked micromanaging kPa; families are readable progression (fd26684) |
| Attitude control | Physical gyro block with saturation | Magic torque deleted; no gyro, no authority (29c9355) |
| Early difficulty | Power scarcity | Hover empties the bank in about 50 s; owner wants it hard at the start |
| Creative mode | exoneer.Creative on during prototype | Flip off before the survival economy test |
| Co-op validation | Target stays 1–4 listen-server; testing deferred | No second tester yet; persistence and JIP are built server-first anyway |

---

## 9. How to use this file

1. Does it make construction, building, driving, or **causal maintenance** more honest? If no, and it is not a project/escape seam, it waits.
2. Does the sandbox still play if the player never accepts a project? If no, reject it.
3. Does a repair verb match the failure, or is it a beam? If beam, reject it.
4. Would a screenshot look like empty Space Engineers, Icarus-the-hunt, or a quest log? If yes, redesign.
5. Is Road to Orbit being faked with a sequence? If yes, slip the feature, do not ship the sequence.
6. Does it add a slider or valve the player must tend every drive? If yes, make it equipment or a talent.

Next work is ROADMAP Phase 1 verification in PIE: P1-P4 code exists and is uncommitted. Then close the two unwired contracts, span load (V-SPAN) and Handshake window (V-HAND). Not more planets, not a campaign script.

---

## 10. Decision log

| Date | Decision | Source |
|---|---|---|
| 2026-08-30 | Nutrition removed; visor HUD code-drawn, no assets | GAME-SCOPE module 1 |
| 2026-09-01 | Bekker-Wong wheel pass chosen over the wrist computer | commits 94ae3ae to 9e39d95 (wheel pass checkpoints 0-3) after the 1a27b89 "V2" baseline |
| 2026-09-01 | Creative mode CVar default on | commit 43230ef |
| 2026-09-02 | Physical gyro block replaces magic rotation torque | commit 29c9355 |
| 2026-09-02 | Tire pressure control removed; four terrain wheel families | commit fd26684 |
| 2026-09-02 | Causal condition, replace verb, project subsystem, save seams landed | working tree, uncommitted |
| 2026-09-03 | Design contract (this doc set) finished | this file |
| 2026-09-03 | ADRs: sandbox-first, no weld-heal, project subsystem | `docs/adr/ADR-001` … `ADR-003` |

Add a row when a decision changes what the game is. Commit messages remain the record of why.
