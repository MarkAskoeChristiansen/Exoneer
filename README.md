# Exoneer

**Exoneer** is the start of a first-person, stylized, survival space-engineering sandbox built for **Unreal Engine 5.7+**. The player is a stranded engineer on an alien planet who survives by mining, refining, fabricating, building modular bases, and eventually constructing vehicles and ships. The visual target is *chunky, colorful, cartoonish sci-fi* with clean silhouettes and rounded edges — never childish, never derivative of any existing IP.

> ⚠️ **Project status.** This repository contains the **C++ foundation, configuration, and project scaffold**. UE5 binary content (Blueprints, maps, UMG widgets, Data Asset instances, meshes, materials) **must be authored inside the UE5 Editor** — it cannot be checked in as text. The first time you open this project in UE5, you'll author the Blueprints/Data Assets/Map listed in [Editor Setup](#editor-setup) below; the C++ systems are already in place and ready to be wired up.

---

## Table of Contents
1. [Design Pillars](#design-pillars)
2. [Project Layout](#project-layout)
3. [Architecture](#architecture)
4. [How to Open & Build](#how-to-open--build)
5. [Editor Setup — bringing the prototype to life](#editor-setup)
6. [Controls](#controls)
7. [Milestones & Roadmap](#milestones--roadmap)
8. [Extending Exoneer](#extending-exoneer)

---

## Design Pillars

| Pillar | What it means in code |
|---|---|
| **Deep engineering, not shallow demo** | Power, oxygen, crafting, conveyor, build, mining, and vehicle systems are all real components, event-driven and data-driven. |
| **Data-driven content** | Every block / item / recipe / biome is a `UPrimaryDataAsset`. Designers can add new content without touching C++. |
| **First-person only (prototype)** | Single `APlayerSurvivalCharacter` pawn, Enhanced Input, FP camera. |
| **Original identity** | Class names, UI, tutorial strings, and art direction are all original. No assets, names, or designs are taken from any existing game. |
| **Scalable foundations** | Grid system, vehicle physics root, planet environment manager, and save subsystem are designed so spherical planets, orbit, and multiplayer can be layered on later without rewrites. |

---

## Project Layout

```
Exoneer.uproject
Config/
  DefaultEngine.ini
  DefaultGame.ini
  DefaultInput.ini
Source/
  Exoneer.Target.cs
  ExoneerEditor.Target.cs
  Exoneer/
    Exoneer.{h,cpp}                          ← module init + log category
    Exoneer.Build.cs
    ExoneerGameMode.{h,cpp}
    Interfaces/                              ← gameplay interfaces (UInterface)
      Interactable.h / Buildable.h / PowerConsumer.h / PowerProducer.h
      OxygenProvider.h / InventoryOwner.h / Pilotable.h / Damageable.h
    Data/                                    ← UPrimaryDataAsset content types
      BlockDefinitionDataAsset.h
      ItemDefinitionDataAsset.h
      RecipeDefinitionDataAsset.h
      PlanetBiomeDataAsset.h
    Components/
      InventoryComponent / CargoComponent
      SurvivalStatsComponent / HealthComponent
      InteractionComponent
      MiningToolComponent / BuildToolComponent
      PowerComponent / PowerNetworkComponent
      OxygenComponent
      ConveyorComponent
      CraftingComponent
    Player/
      PlayerSurvivalCharacter
      FirstPersonEngineerController
    Building/
      BuildableBlock          ← every placed block (base class)
      BlockGridActor          ← owns the cell-dictionary + power network
    Machines/                  ← concrete machine block actors
      MachineBlock (base)
      RefineryBlock / FabricatorBlock / OxygenGeneratorBlock
      BatteryBlock / SolarPanelBlock
      CockpitBlock / ThrusterBlock
    Vehicles/
      VehicleGridActor        ← physics-simulating grid for rovers/ships
    Resources/
      ResourceNode            ← mineable stone/ice/ore deposit
    World/
      PlanetEnvironmentManager ← day/night, temperature, storms, sun fraction
    Save/
      ExoneerSaveGame
      SaveGameSubsystem
Content/Exoneer/
  Blueprints/ Maps/ UI/ Data/ Materials/ Meshes/   (Editor-authored content)
```

---

## Architecture

### Block grids
Two key actors form the spine of construction:

- **`ABlockGridActor`** – a `TMap<FIntVector, ABuildableBlock*>` of cells. Handles `WorldToCell`, `CellToWorldTransform`, `CanPlaceBlock`, `PlaceBlock`, `RemoveBlockAt`. Owns a `UPowerNetworkComponent` that ticks the grid's electrical simulation every 200 ms.
- **`AVehicleGridActor`** *(subclass)* – adds a physically-simulated static mesh root, hover pad logic, and per-tick thrust routing. The active cockpit forwards player input which the vehicle translates into per-thruster throttles and torque on the rigid body.

### Blocks
`ABuildableBlock` is the base placed actor. Concrete machine blocks subclass `AMachineBlock`, which itself adds `UPowerComponent`, `UInventoryComponent`, and `UConveyorComponent`. Refinery / Fabricator / OxygenGenerator further add a `UCraftingComponent` configured for the matching `EExoneerRecipeStation`.

### Power
- `UPowerComponent` lives on each block: `NominalDraw`, `NominalOutput`, `StorageCapacity`, `StoredEnergy`, `SupplyFraction`.
- `UPowerNetworkComponent` (on the grid) sums producers and demand, discharges batteries to cover deficits, charges them with surplus, and writes back a single `SupplyFraction` to every consumer. The result is exposed via `FPowerNetworkSnapshot` (TotalProduction, TotalDemand, Stored, Storage, bOverload) — feed this straight into your power-status HUD.

### Crafting / Refining
`UCraftingComponent` is a generic recipe queue. It only accepts recipes whose `EExoneerRecipeStation` matches the host machine. Each tick of a queued recipe progresses by `DeltaSeconds * SpeedMultiplier * PowerSupplyFraction` so under-powered machines work slower instead of failing silently.

### Survival
`USurvivalStatsComponent` drains Oxygen, SuitPower, Nutrition over time and equilibrates body temperature toward the ambient set by the `APlanetEnvironmentManager`. When a stat hits zero, the component calls `IDamageable::ApplyExoneerDamage` on its owner with the matching `EExoneerDamageType` — so the *same* damage pipeline is used for combat, suffocation, frostbite, etc.

### Mining
`UMiningToolComponent` sweeps a sphere from the camera, damages any `AResourceNode` it hits, and pumps the node's yield into the owning inventory at a rate proportional to damage. Drain on the suit power discourages infinite mining without a charged base.

### Building flow
1. Player toggles to **Build** tool (`IA_ToggleTool`) → `BuildToolComponent::SetBuildModeEnabled(true)`.
2. Player picks a block from the build menu UI → `SetSelectedBlock(...)`.
3. The build tool ticks a ghost preview that snaps to the targeted grid cell, colored by `ValidPreviewMaterial` / `InvalidPreviewMaterial`. `OnBuildPreviewChanged(bValid, EBuildPlacementError)` fires whenever validity changes so the HUD can show "Not enough components", "No support", etc.
4. Player presses `IA_ConfirmPlace`. The tool consumes build cost from the inventory and asks the grid to spawn the actor.

### Save/Load
`USaveGameSubsystem` is a `UGameInstanceSubsystem`. It enumerates the player, every `ABlockGridActor`, and the environment manager, serialising into `UExoneerSaveGame` (player position + survival + inventory; per-block grid: transform, blocks, per-block inventory, stored energy, stored oxygen; environment time-of-day). Reconstruction of blocks on load requires resolving `BlockId → UBlockDefinitionDataAsset` — recommended approach is to register all block definitions as `PrimaryAssetType=Block` via the AssetManager (Edit → Project Settings → AssetManager → Primary Asset Types) and look them up on load.

---

## How to Open & Build

**Requirements**
- Unreal Engine **5.7** or newer
- Visual Studio 2022 (Windows) or Xcode (Mac) with C++ workload
- The `EnhancedInput`, `CommonUI`, and `ChaosVehiclesPlugin` engine plugins (already declared in `Exoneer.uproject`)

**Steps**
1. Clone this repository.
2. Right-click `Exoneer.uproject` → **Generate Visual Studio project files**.
3. Open the generated `Exoneer.sln` and build the **Development Editor / Win64** target. (Or just double-click `Exoneer.uproject` and let the Editor compile on first launch.)
4. UE5 will open with an empty default map — see [Editor Setup](#editor-setup) below.

---

## Editor Setup

This is the canonical recipe for turning the C++ scaffold into a playable vertical slice. Allow ~1 hour for a first pass.

### A. Enhanced Input
Create assets under `/Content/Exoneer/Input/`:

| Asset | Type | Notes |
|---|---|---|
| `IMC_PlayerDefault` | Input Mapping Context | Holds all the bindings below |
| `IA_Move` | Input Action, Axis2D | WASD ↔ value (X,Y) |
| `IA_Look` | Input Action, Axis2D | Mouse XY |
| `IA_Jump` | Input Action, Digital | Space |
| `IA_Sprint` | Input Action, Digital | Left Shift, "Hold" trigger |
| `IA_Crouch` | Input Action, Digital | C |
| `IA_Interact` | Input Action, Digital | E |
| `IA_PrimaryAction` | Input Action, Digital | LMB |
| `IA_SecondaryAction` | Input Action, Digital | RMB |
| `IA_OpenInventory` | Input Action, Digital | Tab |
| `IA_OpenBuildMenu` | Input Action, Digital | B |
| `IA_RotateBlock` | Input Action, Digital | R |
| `IA_ConfirmPlace` | Input Action, Digital | LMB while in build mode |
| `IA_CancelPlace` | Input Action, Digital | Esc |
| `IA_ToggleTool` | Input Action, Digital | Q |
| `IA_EnterExitCockpit` | Input Action, Digital | F |

### B. Player & GameMode Blueprints
Create:
- `BP_PlayerSurvivalCharacter` (parent: `PlayerSurvivalCharacter`).
  - In the **Defaults** panel, assign `DefaultMappingContext = IMC_PlayerDefault` and every `IA_*` slot to the matching Input Action asset above.
- `BP_FirstPersonEngineerController` (parent: `FirstPersonEngineerController`) — leave as default.
- `BP_ExoneerGameMode` (parent: `ExoneerGameMode`). Set:
  - **Default Pawn Class** → `BP_PlayerSurvivalCharacter`
  - **Player Controller Class** → `BP_FirstPersonEngineerController`
  - **HUD Class** → your HUD widget (see UMG step).

In **Project Settings → Maps & Modes**, set the Default GameMode to `BP_ExoneerGameMode`. (The `Config/DefaultEngine.ini` already points at the script-class — the Blueprint version is what you'll actually use.)

### C. Data Assets

Author one **Item Definition Data Asset** for each of (at minimum):
`stone`, `ice`, `iron_ore`, `silicon_ore`, `carbon`, `scrap`, `iron_ingot`, `silicon_wafer`, `plate`, `motor`, `computer_board`, `oxygen` (consumable).

Author one **Block Definition Data Asset** per block listed in the spec. For each, set:
- `BlockId`, `DisplayName`, `Category`, `SizeClass`, `GridSize`, `Mass`, `MaxHealth`
- `BuildCost` (FInventoryEntry list pointing at Item Definitions)
- `PowerDelta` (negative for consumers, positive for producers)
- `OxygenProduction`, `FuelCapacity`, `InventoryCapacity` where relevant
- `BlockActorClass` — choose:
  - `ABuildableBlock` for structural blocks (floor, wall, foundation, …)
  - `ABatteryBlock`, `ASolarPanelBlock`, `ARefineryBlock`, `AFabricatorBlock`, `AOxygenGeneratorBlock`, `ACockpitBlock`, `AThrusterBlock` for the matching machines
  - A custom BP subclass for door/airlock/cargo/etc.
- `PreviewMesh` — a placeholder static mesh (engine cube/cylinder is fine for prototype)
- `Icon` — placeholder texture

Author **Recipe Definition Data Assets** for the production loop:
- `recipe_iron_ingot` (Refinery): 2 iron_ore → 1 iron_ingot, ProcessTime 4s, PowerCost 200
- `recipe_silicon_wafer` (Refinery): 2 silicon_ore → 1 silicon_wafer
- `recipe_plate` (Fabricator): 2 iron_ingot → 1 plate
- `recipe_motor` (Fabricator): 1 iron_ingot + 1 plate → 1 motor
- `recipe_computer_board` (Fabricator): 1 silicon_wafer + 1 plate → 1 computer_board
- `recipe_oxygen` (OxygenGenerator): 1 ice → 5 oxygen

Author one **Planet Biome Data Asset** (`Biome_StarterTundra`) — pick gravity, day/night temps, wind speed, resource spawn list, sky colors.

> 💡 In **Project Settings → AssetManager → Primary Asset Types** add entries for `Item`, `Block`, `Recipe`, `Biome` mapping to the corresponding C++ classes. This lets the save subsystem and any future content browsers resolve assets by `FName`.

### D. UMG Widgets
Create under `/Content/Exoneer/UI/`:
- `WBP_HUD` — root widget. Bind progress bars to `Survival->OnOxygenChanged`, `OnSuitPowerChanged`, `OnNutritionChanged`, `OnTemperatureChanged`, plus `HealthC->OnHealthChanged`. Add labels for `Inventory->GetLoadFraction()`, current tool (`ToolMode`), and selected block (`BuildTool->GetSelectedBlock()`). Bind to `BuildTool->OnBuildPreviewChanged` to display placement error text.
- `WBP_Inventory` — list view over `Inventory->GetEntries()` filtered by category.
- `WBP_BuildMenu` — grid of buttons, one per Block Definition. On click: `PlayerCharacter->SetSelectedBuildBlock(Block)` then `PlayerCharacter->BuildTool->SetBuildModeEnabled(true)`.
- `WBP_MachineUI` — base widget reused by refinery/fabricator/oxygen generator. Lists known recipes (matching the host's `StationType`). "Queue" buttons call `Crafting->Enqueue(Recipe)`.
- `WBP_PowerStatus`, `WBP_OxygenStatus` — bind to `UPowerNetworkComponent::OnPowerNetworkUpdated` and `UOxygenComponent::OnOxygenChanged` respectively.
- `WBP_PauseMenu`, `WBP_Settings` — placeholders.

Implement `RequestOpenInventoryUI`, `RequestOpenBuildMenuUI`, `RequestEnterExitCockpit` (already declared `BlueprintImplementableEvent` on `BP_PlayerSurvivalCharacter`) to add the corresponding widgets to the viewport.

### E. Starter Map (`L_StarterPlanet`)

1. Create a level under `/Content/Exoneer/Maps/L_StarterPlanet`.
2. Add a stylized landscape (a flat plane sculpted into hills works for the prototype) and a `BP_Sky` (simple sphere with a stylized cloudy gradient material). Add a `DirectionalLight` (assign it to the environment manager below), a `SkyAtmosphere`, an `ExponentialHeightFog`, and a `SkyLight`.
3. Place a `BP_PlanetEnvironmentManager` (parent: `PlanetEnvironmentManager`). Assign your biome data asset and drag the DirectionalLight into the `SunLight` slot.
4. Place a `BP_BlockGridActor` (parent: `BlockGridActor`, **NOT** the vehicle variant). This is the world's base grid. Place it near the spawn so the first blocks snap on top of it.
5. Place an `AResourceNode` for each starter deposit (`stone`, `ice`, `iron_ore`, `silicon_ore`, `carbon`). Use placeholder static meshes; tint the materials per resource. Set the `Yield` reference to the matching Item Definition.
6. Place a "crash pod" — any chunky stylized mesh — at the player spawn (`PlayerStart`). You can give it an `Interactable` child blueprint that grants the player starter components when touched.
7. Set the level's **World Settings → Game Mode Override** to `BP_ExoneerGameMode`.

Hit Play. You should be able to walk, look, mine resources, see your survival stats drain, open the inventory, pick a block in the build menu, see the preview, and place blocks on the grid.

### F. Wiring a vehicle (Milestone 2)

1. Place a `BP_VehicleGridActor` (parent: `VehicleGridActor`) on a flat area.
2. Build (or place via Editor) one cockpit, one battery, one or two thrusters facing different axes, and at least one solar panel (for power).
3. Approach the cockpit and press **F** (`IA_EnterExitCockpit`). The cockpit's `IPilotable::EnterPilot` registers the player; the player's `Move` and `Look` axes are forwarded to the vehicle as `MoveInput`/`RotateInput`.
4. Throttle is computed per thruster based on alignment with the desired velocity, so adding more thrusters in more directions gives you more agility — exactly the engineering loop the spec calls for.

### G. Save/Load
Bind UI buttons to `GetGameInstance()->GetSubsystem<USaveGameSubsystem>()->SaveToSlot("Slot1")` / `LoadFromSlot("Slot1")`. Re-spawning grids requires the AssetManager registration in step C; see the comment at the bottom of `SaveGameSubsystem::ApplyWorldState`.

---

## Controls

| Action | Default key |
|---|---|
| Move | WASD |
| Look | Mouse |
| Jump | Space |
| Sprint (hold) | Left Shift |
| Crouch (toggle) | C |
| Interact | E |
| Primary action (mine / place / repair) | Left Mouse |
| Secondary action (build mode: remove) | Right Mouse |
| Open inventory | Tab |
| Open build menu | B |
| Rotate block | R |
| Confirm placement | Left Mouse (in build mode) |
| Cancel placement | Esc |
| Toggle tool (Mining ↔ Build ↔ Repair) | Q |
| Enter / exit cockpit | F |

(These are the suggested bindings — the actual mappings live on `IMC_PlayerDefault` and are editable in the UE5 Editor.)

---

## Milestones & Roadmap

### ✅ Milestone 0 — Scaffold (this repo)
- All systems described above exist as compilable C++.
- Project compiles against UE 5.7 with Enhanced Input + CommonUI + ChaosVehicles.

### 🎯 Milestone 1 — First Survival Loop
After [Editor Setup](#editor-setup): walk a planet surface, mine resources, manage oxygen / suit power, open inventory, craft basic components, place modular blocks, and build a powered base with solar panel, battery, oxygen generator, refinery, fabricator, cargo container.

### 🎯 Milestone 2 — First Vehicle
Build and pilot a small modular hover-rover or atmospheric craft using cockpit + power + thrusters + mass-based movement (`AVehicleGridActor`).

### 🎯 Milestone 3 — Persistence + Hazards + Space prep
- Save / load slots wired into UI.
- Storm and night events reduce solar yield and harm an exposed player (already simulated by `APlanetEnvironmentManager` + `USurvivalStatsComponent`).
- Add a "launch pad" placeholder that streams a second level (the orbit transition path).

### Beyond
- True spherical planet (swap `APlanetEnvironmentManager` for a surface-walking + gravity-direction-rotation system; everything else is already grid-relative).
- Pressurized rooms (add a `UPressureZoneComponent` that walks `ConnectionPoints` of placed blocks).
- Modular ship combat, multiplayer, and persistent universe.

---

## Extending Exoneer

### Add a new block
1. Create a new `UBlockDefinitionDataAsset` instance.
2. Set its `BlockActorClass` to `ABuildableBlock` (or one of the machine subclasses if it needs power/inventory).
3. Add an entry to your build menu widget. Done — no C++ changes needed.

### Add a new resource
1. Create a `UItemDefinitionDataAsset`.
2. Drop a `BP_ResourceNode` in the map and set its `Yield` to the new item.
3. (Optional) Add a recipe asset that consumes the new resource.

### Add a new machine type
1. Subclass `AMachineBlock` in C++ (e.g. `AHydroponicsBlock`).
2. Add a `UCraftingComponent` with `StationType = FoodPrinter`.
3. Create recipes targeting `EExoneerRecipeStation::FoodPrinter`.
4. Author a Block Definition pointing `BlockActorClass` at the new class.

### Add a new planet/biome
Author a new `UPlanetBiomeDataAsset` and a new map with a `BP_PlanetEnvironmentManager` referencing it. Different biomes give you different gravity, temperature curves, wind (and thus turbine yield), storm probability, and resource distributions.

---

## License

This project is provided under the MIT License — see `LICENSE` if/when added. All names, lore, and visual direction are original to Exoneer.
