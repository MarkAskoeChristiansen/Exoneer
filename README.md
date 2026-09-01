# Exoneer

**Exoneer** is a first-person, stylized, survival space-engineering sandbox built for **Unreal Engine 5.8+**. The player is a stranded engineer on an alien planet who survives by mining, refining, fabricating, building modular bases, and constructing physics-driven vehicles. The visual target is *chunky, colorful, cartoonish sci-fi* with clean silhouettes and rounded edges — never childish, never derivative of any existing IP.

> **Project status.** This repository contains the **C++ foundation (construction layer v2), configuration, and project scaffold**, designed for 1–4 player listen-server co-op: every gameplay system is server-authoritative and replicated. UE5 binary content (Blueprints, maps, UMG widgets, Data Asset instances, meshes, materials) must be authored inside the UE5 Editor — see [Editor Setup](#editor-setup).
>
> **The permanent design boundary is [docs/GAME-SCOPE.md](docs/GAME-SCOPE.md)** (master game scope architecture v2.0 — eight modules, hyper-realistic engineering rules, no arcade stat boosts, diegetic UI only). The current implementation layer is specified in [docs/ARCHITECTURE-V2.md](docs/ARCHITECTURE-V2.md); its gap map against the scope is in GAME-SCOPE.md section 9.

---

## Design pillars

| Pillar | What it means in code |
|---|---|
| **Two construction fantasies, one game** | Bases are socket-snapped architectural pieces with material tiers and structural support rules. Vehicles are volumetric blocks on a unified 25 cm grid forming one rigid body, with emergent mass, center of mass, and thrust. |
| **Ghost, then invest** | Everything is placed as a free ghost frame and finished by welding materials into it. Co-op friendly: any player can weld any ghost. |
| **Server-authoritative co-op** | All mutations run on the server; clients send validated intents. Inventories and vehicle blocks replicate through fast array serializers. |
| **Data-driven content** | Every piece / vehicle block / item / recipe / biome is a `UPrimaryDataAsset`. Designers add content without touching C++. |
| **Low coupling** | UInterfaces (`IInteractable`, `IConstructible`, `IDamageable`, `IPilotable`) + native Gameplay Tags for verbs and mount compatibility. |

---

## Architecture (short version)

The authoritative reference is [docs/ARCHITECTURE-V2.md](docs/ARCHITECTURE-V2.md). The spine:

### Base building (socket snapping)
- **`ABaseStructure`** owns the piece registry, the socket occupancy graph, the BFS structural support solver, and a `UPowerNetworkComponent`.
- **`ABasePiece`** is one placed piece (foundation, wall, floor, ramp, roof, beam). Its `UPieceDefinitionDataAsset` declares sockets (`FPieceSocketDef`: local transform + accepted mount tags), material tier, staged build costs, support budget/cost, and storm resistance.
- Pieces without support collapse in batches. Grounded pieces (foundations, beams) seed the support solver.
- **`AMachinePiece`** extends `ABasePiece` with power, an internal inventory, a conveyor link, and a replicated `EMachineState` (Idle / Processing / OutputFull / LowPower) for UI and VFX. Concrete machines: Refinery, Fabricator, OxygenGenerator, Battery, SolarPanel.

### Vehicle building (unified grid)
- **`AVehicleConstruct`** is one physics-simulated rigid body plus a fast-array of `FVehicleBlockRecord`s (25 cm cells, 24 orientations — see `VehicleOrientation.h`). Each block contributes a welded collision box with a per-block mass override, so mass and handling emerge from the layout.
- Functional blocks (thruster, cockpit, battery, solar) spawn server-side `UVehicleModule` objects. A per-tick power ledger produces `PowerSupplyFraction`, which scales thrust — under-powered craft fly sluggishly instead of failing.
- Removing blocks runs flood-fill split detection: disconnected islands become new constructs.
- Piloting: interact with a cockpit; your character stays possessed and forwards move/look intents through a server RPC.

### Construction flow
- `UBuildToolComponent` previews placements client-side (socket snap or grid snap), commits through validated server RPCs, and welds/deconstructs via `IConstructible`.
- `UConstructionComponent` (base pieces) and the block records (vehicles) share the same staged ghost → invest → complete model with proportional material consumption and refunds.

### Survival loop
- `UInventoryComponent`: replicated stacks (weight-based for the suit, volume-based for cargo/machines), server-validated container transfers.
- `UMiningToolComponent` → `AResourceNode`: client beam intent, server-validated damage/yield, replicated node integrity.
- `APlanetEnvironmentManager`: replicated day/night and storms; storms damage exposed completed pieces, mitigated by tier storm resistance.
- `USaveGameSubsystem`: server-only save/load of structures, vehicles, player, and environment; definitions resolve via AssetManager primary asset ids.

---

## How to open & build

**Requirements**
- Unreal Engine **5.8** or newer
- Visual Studio 2022+ with the C++ workload
- The `EnhancedInput`, `CommonUI`, and `ChaosVehiclesPlugin` engine plugins (declared in `Exoneer.uproject`)

**Steps**
1. Clone this repository.
2. Right-click `Exoneer.uproject` → **Generate Visual Studio project files**.
3. Open `Exoneer.sln` and build **Development Editor / Win64** (or double-click the .uproject and let the editor compile).

---

## Editor setup

Bringing the prototype to life takes roughly an hour of editor work. Asset locations follow `/Content/Exoneer/...`.

### A. Enhanced Input
Same action set as before: `IMC_PlayerDefault` plus `IA_Move`, `IA_Look`, `IA_Jump`, `IA_Sprint`, `IA_Crouch`, `IA_Interact`, `IA_PrimaryAction`, `IA_SecondaryAction`, `IA_OpenInventory`, `IA_OpenBuildMenu`, `IA_RotateBlock`, `IA_ConfirmPlace`, `IA_CancelPlace`, `IA_ToggleTool`, `IA_EnterExitCockpit`.

### B. Player & GameMode Blueprints
- `BP_PlayerSurvivalCharacter` (parent `PlayerSurvivalCharacter`): assign the IMC and every `IA_*` slot.
- `BP_ExoneerGameMode`: Default Pawn + `FirstPersonEngineerController` + your HUD.
- Project Settings → Maps & Modes → default GameMode.

### C. Data assets
In **Project Settings → AssetManager → Primary Asset Types**, register: `Item`, `Piece`, `VehicleBlock`, `Recipe`, `Biome` → mapped to the matching `U*DataAsset` classes. Save/load and menu lookups depend on this.

- **Items** (`UItemDefinitionDataAsset`): `stone`, `ice`, `iron_ore`, `silicon_ore`, `carbon`, `scrap`, `iron_ingot`, `silicon_wafer`, `plate`, `motor`, `computer_board`, `oxygen`.
- **Pieces** (`UPieceDefinitionDataAsset`): per tier (Salvage/Alloy/Composite): foundation, wall, floor, ramp, roof, beam — set `MountTag`, sockets with `AcceptedMounts`, `Stages` (build costs + weld work), support budget/cost, `bGroundable` on foundations/beams. Machines: refinery, fabricator, oxygen generator, battery, solar panel — `MountTag = Exoneer.Mount.Deployable`, `PieceClass` = the matching `A*Piece`, machine stats (`PowerDelta`, `EnergyStorage`, `InventoryCapacity`, `OxygenProductionPerSec`).
- **Vehicle blocks** (`UVehicleBlockDefinitionDataAsset`): `frame_1x1` (structural), `cockpit`, `thruster_small`, `battery_small`, `solar_small` — set `SizeInCells`, `Mass`, `Stages`, `ModuleClass` (`UCockpitModule`, `UThrusterModule`, `UBatteryModule`, `USolarModule`), `PowerDelta` / `EnergyStorage` / `MaxThrust`.
- **Recipes**: as before (`recipe_iron_ingot`, `recipe_silicon_wafer`, `recipe_plate`, `recipe_motor`, `recipe_computer_board`, `recipe_oxygen`).
- **Biome** (`Biome_StarterTundra`): gravity, temps, wind, resource list, `StormProbabilityPerHour`, `StormDamagePerSecond`.

### D. UMG widgets
- `WBP_HUD`: survival bars, tool mode, selected buildable (`BuildTool->GetSelected()`), placement error text via `BuildTool->OnBuildPreviewChanged`, interaction prompt + verb tags via `Interactor->GetFocusedInteractionTags()`.
- `WBP_Inventory`: list over `Inventory->GetStacks()`; move items between containers with `Inventory->RequestTransfer(...)` (call it on the LOCAL player's inventory component).
- `WBP_BuildMenu`: one button per Piece/VehicleBlock definition → `SetSelectedPiece(...)` / `SetSelectedVehicleBlock(...)`.
- `WBP_MachineUI`: recipes filtered by the machine's `StationType`; queue via `Interactor->RequestEnqueueRecipe(Machine->Crafting, Recipe)` (client-safe path). Open it from `AMachinePiece::OpenMachineUI` (fires on the interacting client).
- `WBP_PowerStatus` / `WBP_OxygenStatus`: bind `UPowerNetworkComponent::OnPowerNetworkUpdated` / `UOxygenComponent::OnOxygenChanged`.

### E. Starter map (`L_StarterPlanet`)
1. Stylized landscape, sky, `DirectionalLight`, `SkyAtmosphere`, fog, skylight.
2. `BP_PlanetEnvironmentManager` with your biome asset and the sun light assigned.
3. `AResourceNode` instances per starter deposit (stone, ice, iron, silicon, carbon) with placeholder meshes; set `Yield` and optionally `YieldMultiplier`.
4. A crash-pod landmark at `PlayerStart`.
5. World Settings → GameMode override.

There is no pre-placed grid actor anymore: the FIRST foundation you place on terrain founds an `ABaseStructure`; the first vehicle frame block placed on terrain founds an `AVehicleConstruct`.

### F. First vehicle
Place frame blocks in Build mode (vehicle selection), weld them, add a cockpit + battery + solar + thrusters, weld those, press **F** at the cockpit. More thrusters in more directions = more agility.

---

## Controls

| Action | Default key |
|---|---|
| Move / Look / Jump / Sprint / Crouch | WASD / Mouse / Space / Shift / C |
| Interact | E |
| Primary action (mine / weld / confirm place) | Left Mouse |
| Secondary action (deconstruct) | Right Mouse |
| Open inventory / build menu | Tab / B |
| Rotate block, cycle socket | R |
| Cancel placement | Esc |
| Toggle tool (Mining ↔ Build ↔ Weld) | Q |
| Enter / exit cockpit | F |

---

## Milestones & roadmap

### Done — construction layer v2 (this repo)
- Replicated inventory/interaction, socket-based base building with structural support, unified-grid physics vehicles with split detection, ghost-then-invest construction, machines with state machines, server-validated mining, replicated environment + storms, save/load v2.

### Next — first co-op survival loop
Editor content pass (assets above), then: walk, mine, refine, fabricate, build a powered base, and weld together a first hover craft — solo and as a 2-player listen-server session.

### Then
- EOS sessions (framework 0): OnlineServicesEOS configuration, lobby + join flow.
- Survival stats replication redo (suit power/oxygen currently sim locally).
- Pressurized rooms, tiered progression, hazards depth, world beauty pass — see the production prompts in the project history.

---

## Extending Exoneer

- **New base piece**: author a `UPieceDefinitionDataAsset`, pick `MountTag` + sockets. No C++.
- **New machine**: subclass `AMachinePiece` (or reuse), author a Deployable piece definition pointing `PieceClass` at it, add recipes for its station.
- **New vehicle block**: author a `UVehicleBlockDefinitionDataAsset`; functional behavior via a `UVehicleModule` subclass.
- **New resource**: item asset + `AResourceNode` in the map with `Yield` set.
- **New biome**: `UPlanetBiomeDataAsset` + a map with a `PlanetEnvironmentManager`.

---

## License

MIT — see `LICENSE` if/when added. All names, lore, and visual direction are original to Exoneer.
