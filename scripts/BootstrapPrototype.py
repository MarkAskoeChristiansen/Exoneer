# Exoneer prototype bootstrap.
# Run inside the UE editor (or via the pythonscript commandlet). Creates all
# editor content the C++ module needs for a playable first loop: Enhanced
# Input assets, item/piece/vehicle-block/recipe/biome data assets, the player
# and game mode Blueprints, and the starter map. Idempotent: existing assets
# are loaded and updated instead of duplicated.

import unreal

ROOT = "/Game/Exoneer"
DIR_INPUT = ROOT + "/Input"
DIR_ITEMS = ROOT + "/Data/Items"
DIR_PIECES = ROOT + "/Data/Pieces"
DIR_BLOCKS = ROOT + "/Data/VehicleBlocks"
DIR_RECIPES = ROOT + "/Data/Recipes"
DIR_BIOMES = ROOT + "/Data/Biomes"
DIR_BP = ROOT + "/Blueprints"
DIR_MAPS = ROOT + "/Maps"

CUBE = unreal.load_asset("/Engine/BasicShapes/Cube")
CYLINDER = unreal.load_asset("/Engine/BasicShapes/Cylinder")
SPHERE = unreal.load_asset("/Engine/BasicShapes/Sphere")

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
eal = unreal.EditorAssetLibrary

log_lines = []
def log(msg):
    print("[Bootstrap] " + msg)
    log_lines.append(msg)

def ensure_data_asset(name, path, klass):
    full = path + "/" + name
    if eal.does_asset_exist(full):
        return unreal.load_asset(full)
    factory = unreal.DataAssetFactory()
    try:
        factory.set_editor_property("data_asset_class", klass)
    except Exception:
        factory.set_editor_property("DataAssetClass", klass)
    asset = asset_tools.create_asset(name, path, klass, factory)
    if asset is None:
        raise RuntimeError("Failed to create asset %s" % full)
    log("created " + full)
    return asset

def set_props(asset, props):
    for key, value in props.items():
        asset.set_editor_property(key, value)

# FGameplayTag is read-only through Python reflection; the C++ bootstrap
# library resolves registered (native) tags by name instead.
def make_tag(name):
    tag = unreal.ExoneerBootstrapLibrary.make_tag(name)
    if not unreal.ExoneerBootstrapLibrary.is_tag_valid(tag):
        raise RuntimeError("Gameplay tag not registered: " + name)
    return tag

def make_tag_container(names):
    return unreal.ExoneerBootstrapLibrary.make_tag_container([unreal.Name(n) for n in names])

def make_key(name):
    try:
        return unreal.Key(key_name=name)
    except Exception:
        key = unreal.Key()
        key.set_editor_property("key_name", name)
        return key

def make_entry(item_asset, count):
    entry = unreal.InventoryEntry()
    entry.set_editor_property("item", item_asset)
    entry.set_editor_property("count", count)
    return entry

def make_stage(materials, weld_work):
    stage = unreal.ConstructionCost()
    stage.set_editor_property("materials", materials)
    stage.set_editor_property("weld_work", weld_work)
    return stage

def make_transform(x, y, z, yaw=0.0):
    t = unreal.Transform()
    t.translation = unreal.Vector(x, y, z)
    t.rotation = unreal.Rotator(roll=0.0, pitch=0.0, yaw=yaw).quaternion()
    t.scale3d = unreal.Vector(1, 1, 1)
    return t

def make_socket(name, x, y, z, accepted, surface=False, yaw=0.0):
    s = unreal.PieceSocketDef()
    s.set_editor_property("socket_name", name)
    s.set_editor_property("local_transform", make_transform(x, y, z, yaw))
    s.set_editor_property("accepted_mounts", make_tag_container(accepted))
    s.set_editor_property("surface_socket", surface)   # C++ bSurfaceSocket; Python strips the b prefix
    return s

# ---------------------------------------------------------------------------
# 1. Enhanced Input
# ---------------------------------------------------------------------------
log("--- input assets ---")

AXIS2D = unreal.InputActionValueType.AXIS2D
BOOL = unreal.InputActionValueType.BOOLEAN

action_specs = {
    "IA_Move": AXIS2D, "IA_Look": AXIS2D, "IA_Jump": BOOL, "IA_Sprint": BOOL,
    "IA_Crouch": BOOL, "IA_Interact": BOOL, "IA_PrimaryAction": BOOL,
    "IA_SecondaryAction": BOOL, "IA_OpenInventory": BOOL, "IA_OpenBuildMenu": BOOL,
    "IA_RotateBlock": BOOL, "IA_ConfirmPlace": BOOL, "IA_CancelPlace": BOOL,
    "IA_ToggleTool": BOOL, "IA_EnterExitCockpit": BOOL,
}
actions = {}
for name, value_type in action_specs.items():
    ia = ensure_data_asset(name, DIR_INPUT, unreal.InputAction)
    ia.set_editor_property("value_type", value_type)
    actions[name] = ia

imc = ensure_data_asset("IMC_PlayerDefault", DIR_INPUT, unreal.InputMappingContext)

def make_modifier(klass):
    return unreal.new_object(klass, outer=imc)

# (action name, key name, modifier list). WASD onto the Axis2D move action:
# X = right, Y = forward, so W/S swizzle the pressed-key scalar into Y.
BINDINGS = [
    ("IA_Move", "W", ["swizzle"]),
    ("IA_Move", "S", ["swizzle", "negate"]),
    ("IA_Move", "D", []),
    ("IA_Move", "A", ["negate"]),
    ("IA_Look", "Mouse2D", []),
    ("IA_Jump", "SpaceBar", []),
    ("IA_Sprint", "LeftShift", []),
    ("IA_Crouch", "C", []),
    ("IA_Interact", "E", []),
    ("IA_PrimaryAction", "LeftMouseButton", []),
    ("IA_SecondaryAction", "RightMouseButton", []),
    ("IA_OpenInventory", "Tab", []),
    ("IA_OpenBuildMenu", "B", []),
    ("IA_RotateBlock", "R", []),
    ("IA_CancelPlace", "X", []),
    ("IA_ToggleTool", "Q", []),
    ("IA_EnterExitCockpit", "F", []),
]

# UE 5.8 moved the runtime mappings into DefaultKeyMappings; the legacy
# 'mappings' array is deprecated and IGNORED at runtime (writing only it left
# every key dead). map_key writes the new storage; modifiers are attached by
# round-tripping the struct, since Python receives copies, not references.
imc.unmap_all()
for action_name, key_name, _mods in BINDINGS:
    imc.map_key(actions[action_name], make_key(key_name))

mods_by_binding = {(a, k): m for a, k, m in BINDINGS if m}
data = imc.get_editor_property("default_key_mappings")
stored = list(data.get_editor_property("mappings"))
for mapping in stored:
    action = mapping.get_editor_property("action")
    key = mapping.get_editor_property("key").get_editor_property("key_name")
    mods = mods_by_binding.get((action.get_name() if action else "", str(key)))
    if mods:
        objs = []
        for mod in mods:
            objs.append(make_modifier(unreal.InputModifierSwizzleAxis if mod == "swizzle" else unreal.InputModifierNegate))
        mapping.set_editor_property("modifiers", objs)
data.set_editor_property("mappings", stored)
imc.set_editor_property("default_key_mappings", data)

# Empty the deprecated array so stale data can never double-fire or confuse.
try:
    imc.set_editor_property("mappings", [])
except Exception:
    pass

check = imc.get_editor_property("default_key_mappings").get_editor_property("mappings")
if len(check) != len(BINDINGS):
    raise RuntimeError("DefaultKeyMappings holds %d of %d bindings" % (len(check), len(BINDINGS)))
log("input mapping context populated (%d runtime mappings)" % len(check))

# ---------------------------------------------------------------------------
# 2. Items
# ---------------------------------------------------------------------------
log("--- items ---")

CAT = unreal.ExoneerItemCategory
item_specs = {
    # id: (display, category, max_stack, mass, volume)
    "stone":          ("Stone", CAT.RAW, 200, 2.0, 1.0),
    "ice":            ("Ice", CAT.RAW, 200, 1.0, 1.0),
    "iron_ore":       ("Iron Ore", CAT.RAW, 200, 3.0, 1.0),
    "silicon_ore":    ("Silicon Ore", CAT.RAW, 200, 2.5, 1.0),
    "carbon":         ("Carbon", CAT.RAW, 200, 1.5, 1.0),
    "scrap":          ("Scrap", CAT.RAW, 200, 2.0, 1.0),
    "iron_ingot":     ("Iron Ingot", CAT.REFINED, 100, 4.0, 0.8),
    "silicon_wafer":  ("Silicon Wafer", CAT.REFINED, 100, 0.5, 0.2),
    "plate":          ("Metal Plate", CAT.COMPONENT, 100, 6.0, 1.2),
    "motor":          ("Motor", CAT.COMPONENT, 50, 8.0, 1.5),
    "computer_board": ("Computer Board", CAT.COMPONENT, 50, 0.5, 0.3),
    "oxygen":         ("Oxygen Canister", CAT.CONSUMABLE, 50, 0.5, 0.5),
}
items = {}
for item_id, (display, cat, stack, mass, vol) in item_specs.items():
    asset = ensure_data_asset("DA_Item_" + item_id, DIR_ITEMS, unreal.ItemDefinitionDataAsset)
    set_props(asset, {
        "item_id": item_id,
        "display_name": unreal.Text(display),
        "category": cat,
        "max_stack": stack,
        "mass": mass,
        "volume": vol,
    })
    items[item_id] = asset

# ---------------------------------------------------------------------------
# 3. Base pieces
# ---------------------------------------------------------------------------
log("--- pieces ---")

M_FOUNDATION = "Exoneer.Mount.Foundation"
M_WALL = "Exoneer.Mount.Wall"
M_ROOF = "Exoneer.Mount.Roof"
M_DEPLOY = "Exoneer.Mount.Deployable"

foundation_sockets = [
    make_socket("Edge_E", 100, 0, 0, [M_FOUNDATION], yaw=0),
    make_socket("Edge_W", -100, 0, 0, [M_FOUNDATION], yaw=180),
    make_socket("Edge_N", 0, 100, 0, [M_FOUNDATION], yaw=90),
    make_socket("Edge_S", 0, -100, 0, [M_FOUNDATION], yaw=-90),
    make_socket("Top_Wall", 0, 0, 100, [M_WALL]),
    make_socket("Surface", 0, 0, 100, [M_DEPLOY], surface=True),
]
wall_sockets = [
    make_socket("Top", 0, 0, 100, [M_WALL, M_ROOF]),
]
roof_sockets = [
    make_socket("Surface", 0, 0, 100, [M_DEPLOY], surface=True),
]

def ensure_piece(name, piece_id, display, mount, sockets, stages, piece_class=None,
                 groundable=False, budget=6, cost=1, health=300.0, mass=150.0,
                 storm_resist=0.1, machine=None):
    asset = ensure_data_asset(name, DIR_PIECES, unreal.PieceDefinitionDataAsset)
    props = {
        "piece_id": piece_id,
        "display_name": unreal.Text(display),
        "tier": unreal.StructureTier.SALVAGE,
        "mount_tag": make_tag(mount),
        "sockets": sockets,
        "stages": stages,
        "max_health": health,
        "mass": mass,
        "support_budget": budget,
        "support_cost": cost,
        "groundable": groundable,   # C++ bGroundable
        "storm_resistance": storm_resist,
        "mesh": CUBE,
        "ghost_mesh": CUBE,
    }
    if piece_class is not None:
        props["piece_class"] = piece_class
    if machine:
        props.update(machine)
    set_props(asset, props)
    return asset

pieces = {}
pieces["foundation"] = ensure_piece(
    "DA_Piece_Foundation", "foundation_salvage", "Salvage Foundation",
    M_FOUNDATION, foundation_sockets,
    [make_stage([make_entry(items["iron_ingot"], 2)], 3.0)],
    groundable=True, budget=8, cost=1, health=500, mass=300)
pieces["wall"] = ensure_piece(
    "DA_Piece_Wall", "wall_salvage", "Salvage Wall",
    M_WALL, wall_sockets,
    [make_stage([make_entry(items["plate"], 1)], 2.5)],
    budget=6, cost=1, health=300, mass=150)
pieces["roof"] = ensure_piece(
    "DA_Piece_Roof", "roof_salvage", "Salvage Roof",
    M_ROOF, roof_sockets,
    [make_stage([make_entry(items["plate"], 1)], 2.5)],
    budget=4, cost=2, health=250, mass=120)

machine_stage = [make_stage([make_entry(items["plate"], 2), make_entry(items["motor"], 1)], 4.0)]
pieces["refinery"] = ensure_piece(
    "DA_Piece_Refinery", "refinery", "Refinery",
    M_DEPLOY, [], machine_stage, piece_class=unreal.RefineryPiece,
    budget=1, cost=1, health=400, mass=400,
    machine={"power_delta": -400.0, "inventory_capacity": 200.0})
pieces["fabricator"] = ensure_piece(
    "DA_Piece_Fabricator", "fabricator", "Fabricator",
    M_DEPLOY, [], machine_stage, piece_class=unreal.FabricatorPiece,
    budget=1, cost=1, health=400, mass=350,
    machine={"power_delta": -300.0, "inventory_capacity": 200.0})
pieces["oxygen_generator"] = ensure_piece(
    "DA_Piece_OxygenGenerator", "oxygen_generator", "Oxygen Generator",
    M_DEPLOY, [], machine_stage, piece_class=unreal.OxygenGeneratorPiece,
    budget=1, cost=1, health=300, mass=250,
    machine={"power_delta": -150.0, "inventory_capacity": 100.0, "oxygen_production_per_sec": 2.0})
pieces["battery"] = ensure_piece(
    "DA_Piece_Battery", "battery", "Battery Bank",
    M_DEPLOY, [], [make_stage([make_entry(items["plate"], 1), make_entry(items["computer_board"], 1)], 3.0)],
    piece_class=unreal.BatteryPiece,
    budget=1, cost=1, health=300, mass=300,
    machine={"energy_storage": 600000.0})
pieces["solar"] = ensure_piece(
    "DA_Piece_Solar", "solar_panel", "Solar Panel",
    M_DEPLOY, [], [make_stage([make_entry(items["silicon_wafer"], 2), make_entry(items["plate"], 1)], 3.0)],
    piece_class=unreal.SolarPanelPiece,
    budget=1, cost=1, health=200, mass=100,
    machine={"power_delta": 900.0})

# ---------------------------------------------------------------------------
# 4. Vehicle blocks
# ---------------------------------------------------------------------------
log("--- vehicle blocks ---")

def ensure_block(name, block_id, display, mesh, stages, module=None, mass=50.0,
                 health=200.0, power=0.0, storage=0.0, thrust=0.0):
    asset = ensure_data_asset(name, DIR_BLOCKS, unreal.VehicleBlockDefinitionDataAsset)
    props = {
        "block_id": block_id,
        "display_name": unreal.Text(display),
        "size_in_cells": unreal.IntVector(1, 1, 1),
        "mass": mass,
        "max_health": health,
        "stages": stages,
        "power_delta": power,
        "energy_storage": storage,
        "max_thrust": thrust,
        "mesh": mesh,
    }
    if module is not None:
        props["module_class"] = module
    set_props(asset, props)
    return asset

# UVehicleModule subclasses are plain UObjects with no Python attribute
# wrapper; resolve their UClasses through the reflection path instead.
def module_class(name):
    cls = unreal.load_class(None, "/Script/Exoneer." + name)
    if cls is None:
        raise RuntimeError("Module class not found: " + name)
    return cls

frame_stage = [make_stage([make_entry(items["iron_ingot"], 1)], 1.5)]
blocks = {}
blocks["frame"] = ensure_block("DA_Block_Frame", "frame_1x1", "Frame Block", CUBE, frame_stage, mass=40)
blocks["cockpit"] = ensure_block(
    "DA_Block_Cockpit", "cockpit", "Cockpit", CUBE,
    [make_stage([make_entry(items["plate"], 2), make_entry(items["computer_board"], 1)], 3.0)],
    module=module_class("CockpitModule"), mass=120, health=300, power=-20.0)
blocks["thruster"] = ensure_block(
    "DA_Block_Thruster", "thruster_small", "Small Thruster", CYLINDER,
    [make_stage([make_entry(items["plate"], 1), make_entry(items["motor"], 1)], 2.5)],
    module=module_class("ThrusterModule"), mass=60, health=200, power=-600.0, thrust=9000.0)
blocks["battery"] = ensure_block(
    "DA_Block_Battery", "battery_small", "Small Battery", CUBE,
    [make_stage([make_entry(items["plate"], 1), make_entry(items["computer_board"], 1)], 2.0)],
    module=module_class("BatteryModule"), mass=80, health=200, storage=900000.0)
blocks["solar"] = ensure_block(
    "DA_Block_Solar", "solar_small", "Solar Collector", CUBE,
    [make_stage([make_entry(items["silicon_wafer"], 1), make_entry(items["plate"], 1)], 2.0)],
    module=module_class("SolarModule"), mass=30, health=120, power=1200.0)

# ---------------------------------------------------------------------------
# 5. Recipes
# ---------------------------------------------------------------------------
log("--- recipes ---")

STATION = unreal.ExoneerRecipeStation
recipe_specs = [
    ("recipe_iron_ingot", "Smelt Iron Ingot", STATION.REFINERY, [("iron_ore", 2)], [("iron_ingot", 1)], 4.0, 200.0),
    ("recipe_silicon_wafer", "Refine Silicon Wafer", STATION.REFINERY, [("silicon_ore", 2)], [("silicon_wafer", 1)], 4.0, 200.0),
    ("recipe_plate", "Press Metal Plate", STATION.FABRICATOR, [("iron_ingot", 2)], [("plate", 1)], 3.0, 150.0),
    ("recipe_motor", "Assemble Motor", STATION.FABRICATOR, [("iron_ingot", 1), ("plate", 1)], [("motor", 1)], 5.0, 200.0),
    ("recipe_computer_board", "Print Computer Board", STATION.FABRICATOR, [("silicon_wafer", 1), ("plate", 1)], [("computer_board", 1)], 5.0, 250.0),
    ("recipe_oxygen", "Electrolyze Oxygen", STATION.OXYGEN_GENERATOR, [("ice", 1)], [("oxygen", 5)], 3.0, 150.0),
]

def make_ingredient(item_id, count):
    ing = unreal.RecipeIngredient()
    ing.set_editor_property("item", items[item_id])
    ing.set_editor_property("count", count)
    return ing

for rid, display, station, inputs, outputs, time, power in recipe_specs:
    asset = ensure_data_asset("DA_Recipe_" + rid, DIR_RECIPES, unreal.RecipeDefinitionDataAsset)
    set_props(asset, {
        "recipe_id": rid,
        "display_name": unreal.Text(display),
        "station": station,
        "inputs": [make_ingredient(i, c) for i, c in inputs],
        "outputs": [make_ingredient(i, c) for i, c in outputs],
        "process_time": time,
        "power_cost": power,
    })

# ---------------------------------------------------------------------------
# 6. Biome
# ---------------------------------------------------------------------------
log("--- biome ---")

biome = ensure_data_asset("DA_Biome_StarterTundra", DIR_BIOMES, unreal.PlanetBiomeDataAsset)
set_props(biome, {
    "biome_id": "starter_tundra",
    "display_name": unreal.Text("Starter Tundra"),
    "gravity_z": -980.0,
    "day_temp_celsius": 18.0,
    "night_temp_celsius": -35.0,
    "storm_probability_per_hour": 0.5,
    "storm_damage_per_second": 2.0,
})

# ---------------------------------------------------------------------------
# 7. Blueprints
# ---------------------------------------------------------------------------
log("--- blueprints ---")

def ensure_blueprint(name, parent):
    full = DIR_BP + "/" + name
    if eal.does_asset_exist(full):
        return unreal.load_asset(full)
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", parent)
    bp = asset_tools.create_asset(name, DIR_BP, unreal.Blueprint, factory)
    if bp is None:
        raise RuntimeError("Failed to create blueprint " + full)
    log("created " + full)
    return bp

bp_char = ensure_blueprint("BP_PlayerSurvivalCharacter", unreal.PlayerSurvivalCharacter)
char_class = eal.load_blueprint_class(DIR_BP + "/BP_PlayerSurvivalCharacter")
char_cdo = unreal.get_default_object(char_class)
char_cdo.set_editor_property("default_mapping_context", imc)
for name in action_specs:
    char_cdo.set_editor_property(name, actions[name])
char_cdo.set_editor_property("quick_bar", [
    pieces["foundation"], pieces["wall"], pieces["roof"],
    pieces["solar"], pieces["battery"], pieces["refinery"],
    pieces["fabricator"], pieces["oxygen_generator"],
    blocks["frame"], blocks["cockpit"], blocks["thruster"],
    blocks["battery"], blocks["solar"],
])
char_cdo.set_editor_property("starter_items", [
    make_entry(items["iron_ingot"], 40),
    make_entry(items["plate"], 30),
    make_entry(items["motor"], 10),
    make_entry(items["silicon_wafer"], 10),
    make_entry(items["computer_board"], 10),
    make_entry(items["ice"], 20),
])

bp_gm = ensure_blueprint("BP_ExoneerGameMode", unreal.ExoneerGameMode)
gm_class = eal.load_blueprint_class(DIR_BP + "/BP_ExoneerGameMode")
gm_cdo = unreal.get_default_object(gm_class)
gm_cdo.set_editor_property("default_pawn_class", char_class)
gm_cdo.set_editor_property("player_controller_class", unreal.FirstPersonEngineerController.static_class())
gm_cdo.set_editor_property("hud_class", unreal.ExoneerHUD.static_class())

# ---------------------------------------------------------------------------
# 8. Starter map
# ---------------------------------------------------------------------------
log("--- map ---")

level_path = DIR_MAPS + "/L_StarterPlanet"
les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

if eal.does_asset_exist(level_path):
    les.load_level(level_path)
    log("loaded existing map")
else:
    les.new_level(level_path)
    log("created map " + level_path)

    def spawn(cls, loc, rot=None):
        return eas.spawn_actor_from_class(cls, unreal.Vector(*loc), rot or unreal.Rotator(0, 0, 0))

    # Ground slab: 4 km x 4 km, top surface at Z = 0.
    ground = spawn(unreal.StaticMeshActor, (0, 0, -52))
    ground_mesh = ground.get_editor_property("static_mesh_component")
    ground_mesh.set_editor_property("static_mesh", CUBE)
    ground.set_actor_scale3d(unreal.Vector(400, 400, 1))
    ground.set_actor_label("GroundSlab")

    # Movable: the environment manager rotates the sun for the day/night cycle.
    sun = spawn(unreal.DirectionalLight, (0, 0, 500), unreal.Rotator(roll=0.0, pitch=-45.0, yaw=30.0))
    sun.root_component.set_mobility(unreal.ComponentMobility.MOVABLE)
    sun.set_actor_label("Sun")
    spawn(unreal.SkyAtmosphere, (0, 0, 0)).set_actor_label("SkyAtmosphere")
    spawn(unreal.SkyLight, (0, 0, 400)).set_actor_label("SkyLight")
    spawn(unreal.ExponentialHeightFog, (0, 0, 0)).set_actor_label("HeightFog")

    env = spawn(unreal.PlanetEnvironmentManager, (0, 0, 0))
    env.set_editor_property("biome", biome)
    env.set_editor_property("sun_light", sun)
    env.set_actor_label("PlanetEnvironmentManager")

    spawn(unreal.PlayerStart, (0, 0, 150)).set_actor_label("PlayerStart")

    node_specs = [
        ("stone", (1200, 400, 60), 2.2),
        ("iron_ore", (1500, -900, 60), 2.5),
        ("iron_ore", (-1400, 1100, 60), 2.5),
        ("silicon_ore", (-900, -1600, 60), 2.2),
        ("carbon", (500, 1900, 60), 2.0),
        ("ice", (-2000, -300, 60), 2.4),
    ]
    for item_id, loc, scale in node_specs:
        node = spawn(unreal.ResourceNode, loc)
        mesh_comp = node.get_editor_property("mesh")
        mesh_comp.set_editor_property("static_mesh", SPHERE)
        node.set_actor_scale3d(unreal.Vector(scale, scale, scale))
        node.set_editor_property("Yield", items[item_id])
        node.set_editor_property("max_integrity", 400.0)
        node.set_actor_label("Node_" + item_id)

    les.save_current_level()
    log("map saved")

# ---------------------------------------------------------------------------
# Save everything
# ---------------------------------------------------------------------------
unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
log("bootstrap complete: %d steps logged" % len(log_lines))
