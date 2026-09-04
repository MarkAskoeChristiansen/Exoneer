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
DIR_PROJECTS = ROOT + "/Data/Projects"
DIR_BP = ROOT + "/Blueprints"
DIR_MAPS = ROOT + "/Maps"
DIR_PHYSMATS = ROOT + "/PhysicalMaterials"
DIR_MATS = ROOT + "/Materials"

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
AXIS1D = unreal.InputActionValueType.AXIS1D
BOOL = unreal.InputActionValueType.BOOLEAN

action_specs = {
    "IA_Move": AXIS2D, "IA_Look": AXIS2D, "IA_Jump": BOOL, "IA_Sprint": BOOL,
    "IA_Crouch": BOOL, "IA_Interact": BOOL, "IA_PrimaryAction": BOOL,
    "IA_SecondaryAction": BOOL, "IA_OpenInventory": BOOL, "IA_OpenBuildMenu": BOOL,
    "IA_RotateBlock": BOOL, "IA_ConfirmPlace": BOOL, "IA_CancelPlace": BOOL,
    # Quick bar split (V-SPAN pass): B cycles base pieces, N vehicle blocks.
    # The one list grew past the point where a single cycle key was usable.
    "IA_CycleVehicleBlock": BOOL,
    "IA_ToggleTool": BOOL, "IA_EnterExitCockpit": BOOL,
    # Piloting extras (wheel pass): service brake, handbrake, Flight/Ground
    # mode toggle, CTIS hold-to-pump tire pressure.
    "IA_Brake": BOOL, "IA_Handbrake": BOOL, "IA_ToggleControlMode": BOOL,
    # Chase-camera zoom while seated. Axis1D: the wheel axis is already
    # signed, so no negate modifier is wanted.
    "IA_CameraZoom": AXIS1D,
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
    ("IA_CycleVehicleBlock", "N", []),
    ("IA_RotateBlock", "R", []),
    ("IA_CancelPlace", "X", []),
    ("IA_ToggleTool", "Q", []),
    ("IA_EnterExitCockpit", "F", []),
    ("IA_Brake", "Z", []),
    ("IA_Handbrake", "LeftControl", []),
    ("IA_ToggleControlMode", "V", []),
    ("IA_CameraZoom", "MouseWheelAxis", []),
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
    "tire":           ("Tire", CAT.COMPONENT, 20, 25.0, 4.0),
    "battery_cell":   ("Battery Cell", CAT.COMPONENT, 50, 12.0, 2.0),
    "oxygen":         ("Oxygen Canister", CAT.CONSUMABLE, 50, 0.5, 0.5),
    "fuel":           ("Propellant", CAT.FUEL, 50, 2.0, 2.0),
    "seal_kit":       ("Seal Kit", CAT.CONSUMABLE, 20, 1.0, 0.5),
    "suit_seal":      ("Suit Seal", CAT.COMPONENT, 10, 2.0, 1.0),
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
M_FLOOR = "Exoneer.Mount.Floor"
M_RAMP = "Exoneer.Mount.Ramp"
M_BEAM = "Exoneer.Mount.Beam"
M_ROOF = "Exoneer.Mount.Roof"
M_DEPLOY = "Exoneer.Mount.Deployable"

foundation_sockets = [
    make_socket("Edge_E", 100, 0, 0, [M_FOUNDATION, M_RAMP], yaw=0),
    make_socket("Edge_W", -100, 0, 0, [M_FOUNDATION, M_RAMP], yaw=180),
    make_socket("Edge_N", 0, 100, 0, [M_FOUNDATION, M_RAMP], yaw=90),
    make_socket("Edge_S", 0, -100, 0, [M_FOUNDATION, M_RAMP], yaw=-90),
    make_socket("Top_Wall", 0, 0, 100, [M_WALL, M_FLOOR, M_BEAM]),
    make_socket("Surface", 0, 0, 100, [M_DEPLOY], surface=True),
]
wall_sockets = [
    make_socket("Top", 0, 0, 100, [M_WALL, M_ROOF, M_FLOOR]),
]
floor_sockets = [
    make_socket("Edge_E", 100, 0, 0, [M_WALL], yaw=0),
    make_socket("Edge_W", -100, 0, 0, [M_WALL], yaw=180),
    make_socket("Edge_N", 0, 100, 0, [M_WALL], yaw=90),
    make_socket("Edge_S", 0, -100, 0, [M_WALL], yaw=-90),
    make_socket("Surface", 0, 0, 0, [M_DEPLOY], surface=True),
]
beam_sockets = [
    make_socket("Top", 0, 0, 200, [M_FLOOR, M_BEAM, M_ROOF]),
]
roof_sockets = [
    make_socket("Surface", 0, 0, 100, [M_DEPLOY], surface=True),
]
# A span chains end to end on the existing 2 m socket module: each end accepts
# another floor-mounted span, so support relaxes along the chain from whatever
# grounded beam anchors the first one.
span_sockets = [
    make_socket("Edge_E", 100, 0, 0, [M_FLOOR], yaw=0),
    make_socket("Edge_W", -100, 0, 0, [M_FLOOR], yaw=180),
]

# load_capacity is a MASS (kg): the weight limit is load_capacity * g on this
# planet. 0 means UNRATED - the piece is not a deck and collapses under any
# wheel - so EVERY piece is authored here and none is left at the default.
def ensure_piece(name, piece_id, display, mount, sockets, stages, piece_class=None,
                 groundable=False, budget=6, cost=1, health=300.0, mass=150.0,
                 storm_resist=0.1, machine=None, load_capacity=0.0,
                 terminal_deflection=60.0, spare_item=None):
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
        "load_capacity_kg": load_capacity,
        "terminal_deflection_mm": terminal_deflection,
        "mesh": CUBE,
        "ghost_mesh": CUBE,
    }
    if piece_class is not None:
        props["piece_class"] = piece_class
    # The fabricated spare the Replace verb consumes. Named explicitly per
    # piece: a piece with no spare can only be rebuilt.
    props["spare_item_id"] = spare_item or ""
    if machine:
        props.update(machine)
    set_props(asset, props)
    return asset

pieces = {}
pieces["foundation"] = ensure_piece(
    "DA_Piece_Foundation", "foundation_salvage", "Salvage Foundation",
    M_FOUNDATION, foundation_sockets,
    [make_stage([make_entry(items["iron_ingot"], 2)], 3.0)],
    groundable=True, budget=8, cost=1, health=500, mass=300, load_capacity=5000)
pieces["wall"] = ensure_piece(
    "DA_Piece_Wall", "wall_salvage", "Salvage Wall",
    M_WALL, wall_sockets,
    [make_stage([make_entry(items["plate"], 1)], 2.5)],
    budget=6, cost=1, health=300, mass=150, load_capacity=400)
pieces["roof"] = ensure_piece(
    "DA_Piece_Roof", "roof_salvage", "Salvage Roof",
    M_ROOF, roof_sockets,
    [make_stage([make_entry(items["plate"], 1)], 2.5)],
    budget=4, cost=2, health=250, mass=120, load_capacity=300)
pieces["floor"] = ensure_piece(
    "DA_Piece_Floor", "floor_salvage", "Salvage Floor",
    M_FLOOR, floor_sockets,
    [make_stage([make_entry(items["plate"], 1)], 2.5)],
    budget=5, cost=1, health=280, mass=140, load_capacity=800)
pieces["ramp"] = ensure_piece(
    "DA_Piece_Ramp", "ramp_salvage", "Salvage Ramp",
    M_RAMP, [
        make_socket("Top", 0, 0, 50, [M_FLOOR, M_FOUNDATION]),
    ],
    [make_stage([make_entry(items["iron_ingot"], 2)], 3.0)],
    groundable=True, budget=6, cost=1, health=320, mass=180, load_capacity=1000)
pieces["beam"] = ensure_piece(
    "DA_Piece_Beam", "beam_salvage", "Salvage Beam",
    M_BEAM, beam_sockets,
    [make_stage([make_entry(items["iron_ingot"], 2)], 3.0)],
    groundable=True, budget=8, cost=1, health=400, mass=200, load_capacity=1500)
pieces["road"] = ensure_piece(
    "DA_Piece_Road", "road_deck", "Road Deck",
    M_FOUNDATION, foundation_sockets,
    [make_stage([make_entry(items["iron_ingot"], 2), make_entry(items["plate"], 1)], 4.0)],
    groundable=True, budget=10, cost=1, health=600, mass=400, storm_resist=0.2,
    load_capacity=1200)
# A light deck is the teaching case for V-SPAN: one axle of the test rover
# (about 9.8 kN) against 600 kg of rating is ratio 1.67, so it sags to its
# 60 mm terminal reading in about 15 s. The same axle on road_deck is 0.83 and
# crosses all day.
pieces["road_light"] = ensure_piece(
    "DA_Piece_RoadLight", "road_deck_light", "Light Road Deck",
    M_FOUNDATION, foundation_sockets,
    [make_stage([make_entry(items["plate"], 1), make_entry(items["stone"], 2)], 3.0)],
    groundable=True, budget=6, cost=1, health=300, mass=200, storm_resist=0.15,
    load_capacity=600)
# Not groundable: a span hangs off a beam top (M_FLOOR mount) and reaches out
# over a gap. Budget 6 against cost 1 relaxes 8, 7, 5, 4, 3, 2, 1 from a
# grounded beam: a seven-piece chain, the beam plus six spans, and the seventh
# span reads 0 and falls. A two-anchor solver stays post-alpha: the support
# graph is still a single-parent tree.
pieces["bridge_span"] = ensure_piece(
    "DA_Piece_BridgeSpan", "bridge_span", "Bridge Span",
    M_FLOOR, span_sockets,
    [make_stage([make_entry(items["iron_ingot"], 3), make_entry(items["plate"], 2),
                 make_entry(items["stone"], 4)], 5.0)],
    budget=6, cost=1, health=450, mass=260, storm_resist=0.15,
    load_capacity=1500)
pieces["shelter"] = ensure_piece(
    "DA_Piece_Shelter", "storm_shelter", "Storm Shelter",
    M_ROOF, roof_sockets,
    [make_stage([make_entry(items["plate"], 3)], 4.0)],
    budget=5, cost=2, health=400, mass=180, storm_resist=0.7, load_capacity=800)
pieces["radio"] = ensure_piece(
    "DA_Piece_Radio", "radio_mast", "Radio Mast",
    M_DEPLOY, [],
    [make_stage([make_entry(items["plate"], 1), make_entry(items["computer_board"], 1)], 3.0)],
    piece_class=unreal.MachinePiece,
    budget=1, cost=1, health=200, mass=80, load_capacity=200,
    machine={"power_delta": -40.0})
pieces["dish"] = ensure_piece(
    "DA_Piece_Dish", "dish_array", "High-Gain Dish",
    M_DEPLOY, [],
    [make_stage([make_entry(items["plate"], 4), make_entry(items["computer_board"], 2), make_entry(items["motor"], 1)], 8.0)],
    piece_class=unreal.MachinePiece,
    budget=1, cost=1, health=250, mass=220, load_capacity=400,
    machine={"power_delta": -120.0})

machine_stage = [make_stage([make_entry(items["plate"], 2), make_entry(items["motor"], 1)], 4.0)]
pieces["refinery"] = ensure_piece(
    "DA_Piece_Refinery", "refinery", "Refinery",
    M_DEPLOY, [], machine_stage, piece_class=unreal.RefineryPiece,
    budget=1, cost=1, health=400, mass=400, load_capacity=800,
    machine={"power_delta": -400.0, "inventory_capacity": 200.0})
pieces["fabricator"] = ensure_piece(
    "DA_Piece_Fabricator", "fabricator", "Fabricator",
    M_DEPLOY, [], machine_stage, piece_class=unreal.FabricatorPiece,
    budget=1, cost=1, health=400, mass=350, load_capacity=800,
    machine={"power_delta": -300.0, "inventory_capacity": 200.0})
pieces["oxygen_generator"] = ensure_piece(
    "DA_Piece_OxygenGenerator", "oxygen_generator", "Oxygen Generator",
    M_DEPLOY, [], machine_stage, piece_class=unreal.OxygenGeneratorPiece,
    budget=1, cost=1, health=300, mass=250, load_capacity=800,
    machine={"power_delta": -150.0, "inventory_capacity": 100.0, "oxygen_production_per_sec": 2.0})
pieces["battery"] = ensure_piece(
    "DA_Piece_Battery", "battery", "Battery Bank",
    M_DEPLOY, [], [make_stage([make_entry(items["plate"], 1), make_entry(items["computer_board"], 1)], 3.0)],
    piece_class=unreal.BatteryPiece,
    budget=1, cost=1, health=300, mass=300, load_capacity=600,
    spare_item="battery_cell",
    machine={"energy_storage": 600000.0})
pieces["solar"] = ensure_piece(
    "DA_Piece_Solar", "solar_panel", "Solar Panel",
    M_DEPLOY, [], [make_stage([make_entry(items["silicon_wafer"], 2), make_entry(items["plate"], 1)], 3.0)],
    piece_class=unreal.SolarPanelPiece,
    budget=1, cost=1, health=200, mass=100, load_capacity=200,
    machine={"power_delta": 900.0})
# The only interactable that connects a suit. Cost matches the other small
# machines (plate 2 + motor 1). PowerDelta is the 600 W consumer draw while
# a connected suit is taking charge; the C++ port property is the source of
# truth and re-applies it in ApplyDefinitionStats.
pieces["umbilical"] = ensure_piece(
    "DA_Piece_UmbilicalPort", "umbilical_port", "Umbilical Port",
    M_DEPLOY, [],
    [make_stage([make_entry(items["plate"], 2), make_entry(items["motor"], 1)], 4.0)],
    piece_class=unreal.UmbilicalPortPiece,
    budget=1, cost=1, health=250, mass=80, load_capacity=200,
    machine={"power_delta": -600.0})

# ---------------------------------------------------------------------------
# 4. Vehicle blocks
# ---------------------------------------------------------------------------
log("--- vehicle blocks ---")

def ensure_block(name, block_id, display, mesh, stages, module=None, mass=50.0,
                 health=200.0, power=0.0, storage=0.0, thrust=0.0,
                 size_in_cells=(1, 1, 1), extra=None):
    asset = ensure_data_asset(name, DIR_BLOCKS, unreal.VehicleBlockDefinitionDataAsset)
    props = {
        "block_id": block_id,
        "display_name": unreal.Text(display),
        "size_in_cells": unreal.IntVector(*size_in_cells),
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
    # Extension seam for block-type-specific fields (wheel spec etc.), same
    # pattern as ensure_piece's machine dict.
    props.update(extra or {})
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
blocks["frame"] = ensure_block("DA_Block_Frame", "frame_1x1", "Frame Block", CUBE, frame_stage, mass=14)
blocks["cockpit"] = ensure_block(
    "DA_Block_Cockpit", "cockpit", "Cockpit", CUBE,
    [make_stage([make_entry(items["plate"], 2), make_entry(items["computer_board"], 1)], 3.0)],
    module=module_class("CockpitModule"), mass=85, health=300, power=-20.0)
# Thruster. throttle_slew_per_sec is the valve rate, and the lift setting
# FOLLOWS its target at this rate in both directions, so thrust cannot step
# from 0 to 24 kN in a frame. The target is three-state: the reserved ceiling
# while the lift key is held, zero while the descend key is held, and the hover
# setting when neither is held and the craft is airborne under a live pilot.
#
# A previous pass made the lift key integrate a LATCHING collective whose zero
# meant HOLD; the craft kept climbing after the pilot released it, with no key
# that closed the valve and no readout of where the lever had been left. Going
# fully binary fixed that and created the opposite problem: a 1.19 TWR craft
# could then only climb at 0.19 g or fall at 1 g, with nothing in between and a
# LIFT readout implying a setting the pilot could not choose. The governor is
# the answer to both - it has a key each way, no integrator, and it releases on
# touchdown, on an input timeout, on leaving the seat and on switching mode.
#
# attitude_trim_fraction is how far the DIFFERENTIAL-THRUST path may bias one
# unit away from the pilot's setting. It has exactly two jobs, and the first is
# the one that decides whether a craft is flyable at all:
#
#   1. Cancelling the craft's STANDING moment. Any thrust layout whose net
#      moment about the centre of mass is non-zero at the pilot's own throttle
#      setting makes a moment that never ends. A reaction wheel can only hold
#      one by winding its rotors, so it fills the momentum store at the
#      moment's own magnitude per second - 1224 N*m against the test rover's
#      1600 N*m*s per axis took pitch out 1.3 s into holding the forward key,
#      and after that nothing could stop the craft rotating. Thrust cancels
#      thrust for free and for ever; rotor momentum is a bank account.
#   2. Unwinding stored momentum, which needs an external torque and so cannot
#      be done by the triad itself.
#
# Attitude TRANSIENTS never come here - the authority of a lift-thruster
# allocator is a hidden function of the throttle setting, it makes no yaw
# moment at all, and every correction would move the altitude.
#
# lift_control_reserve_fraction 0.10 is what keeps job 1 possible at the top of
# the lever. A trim bias is symmetric (a unit may only be biased as far as it
# can be biased back), so a valve pinned at 1.00 has ZERO trim authority, and
# full collective is exactly where a climbing pilot sits. Holding the pilot's
# travel to 0.90 leaves each of the six lift units a tenth of its travel either
# way, which is about 1200 N*m of pitch trim and 890 N*m of roll trim even at
# full climb. The price is honest and visible on the visor: ascent TWR is
# 0.90 x 24.0 kN over 18.1 kN = 1.19 rather than 1.32, so the rover climbs at
# 0.19 g instead of 0.32 g. Nothing can buy authority at the BOTTOM of the
# travel: a shut valve makes no thrust and therefore no moment.
#
# lift_hover_damping_per_ms 0.10 is the lift governor's only gain. Release both
# lift keys airborne and the valve seeks weight over world-vertical lift plus
# this gain times the vertical speed, which closes the vertical loop with a
# time constant of mass / (lift scale x gain) = 1849 / 2400 = 0.77 s: the craft
# settles onto zero climb in about two seconds instead of porpoising. There is
# no position term on purpose - an altitude integrator is a latch, and the
# owner has already been given one of those once. With the valve as the only
# actuator, a craft that cannot hover simply runs it to the stop and sinks.
# nozzle_cant_deg 6.0 is the one number that makes yaw a controllable axis in
# the air. Six thrusters pointing along the hull's own up axis make EXACTLY
# zero yaw moment however they are throttled: r x F has no vertical component
# when F is vertical. So before this the only yaw effector on the rover was the
# forward pair, and a forward unit's trim bound is capped by its own base
# throttle - which is zero unless the pilot happens to be holding W.
#
# That mattered because holding a yaw rate is not free. A hull with angular
# damping 0.15 needs 0.15 * 1544 * 0.35 = 81 N*m to STAY at 20 deg/s, for ever,
# and a reaction wheel pays that in momentum every second. The axis therefore
# filled on a stopwatch and the mouse went dead in that direction until the
# pilot yawed back or landed.
#
# Canted 6 degrees and TOED OUTBOARD on each rail (the spawner picks the
# orientation, and the two rails lean opposite ways), the geometry is exactly
# what is wanted: uniform throttle makes no yaw at all, because the rails
# cancel, so there is no windmill and no standing yaw moment; a DIAGONAL trim
# across the four corner units - front-left and rear-right up, front-right and
# rear-left down - is a pure yaw couple with zero net force, zero pitch and
# Measured at the hover collective: 251 N*m of pure yaw at a 0.20 diagonal bias
# and 302 N*m at the full travel that collective leaves, against the 81 N*m the
# axis needs to hold its fastest commanded turn (0.15 x 1544 x 20 deg/s).
#
# THE CANT IS ON THE DEFINITION, SO EVERY THRUSTER HAS IT WHETHER IT WANTS IT
# OR NOT, and the only question a placement can answer is which way the jet
# leans. That is not optional care: the forward-facing pair was placed with a
# one-axis orientation lookup, which leaned both units the SAME way, and two
# 418 N side thrusts that do not cancel 1.277 m behind the centre of mass are
# 920 N*m of standing YAW at the 0.90 ceiling against about 400 N*m of yaw trim
# authority. Measured: 469 N*m into the rotors at hover, 624 N*m at the
# ceiling, the yaw axis full 3.3 s after the pilot first held W, and the hull
# then spinning up to 114 deg/s with the stick centred. Both the lift rails and
# the forward pair are now MIRRORED PAIRS, and the build tool's aim list offers
# the same two-axis choice (THRUST: UP TOE L / TOE R) so a player can build the
# balanced layout the spawner builds. Standing yaw on the shipped craft is
# -41 N*m and the residual after trim is 0 N*m on all three axes.
#
# The cost is honest and visible: world-vertical lift falls by cos^2 of the
# cant, because the collective is shared out in proportion to each unit's lift
# share and then only that share pushes up. 6 degrees costs 1.1 percent -
# 24.00 kN of installed lift becomes 23.74 kN of vertical lift, so the reserved
# ascent TWR is 1.18 rather than 1.19 and the rover climbs at 1.75 m/s^2.
#
# lift_descent_rate_ms 2.5 makes the descend key a RATE COMMAND rather than a
# valve kill. Killing the valve made the only descent control unrecoverable:
# 1 g of fall against 0.19 g of arrest authority is an asymmetry of five to
# one, so four seconds of held Ctrl reached 37 m/s and needed 26 s and 526 m to
# stop, while landing damage starts at 8 m/s and arrived 0.8 s in. Governed to
# a bounded rate the same four seconds reach 2.5 m/s and 7.9 m, and releasing
# levels off in 4.5 s and 10 m. It also keeps the valve off its bottom stop,
# which keeps the trim path alive - Ctrl and W together used to be the one
# state with no trim authority at all.
blocks["thruster"] = ensure_block(
    "DA_Block_Thruster", "thruster_small", "Small Thruster", CYLINDER,
    [make_stage([make_entry(items["plate"], 1), make_entry(items["motor"], 1)], 2.5)],
    module=module_class("ThrusterModule"), mass=45, health=200, power=-12000.0, thrust=4000.0,
    extra={"throttle_slew_per_sec": 2.0, "attitude_trim_fraction": 0.35,
           "lift_control_reserve_fraction": 0.10,
           "lift_hover_damping_per_ms": 0.10,
           "nozzle_cant_deg": 6.0,
           "lift_descent_rate_ms": 2.5})
blocks["battery"] = ensure_block(
    "DA_Block_Battery", "battery_small", "Small Battery", CUBE,
    [make_stage([make_entry(items["plate"], 1), make_entry(items["computer_board"], 1)], 2.0)],
    module=module_class("BatteryModule"), mass=50, health=200, storage=900000.0,
    extra={"spare_item_id": "battery_cell"})
blocks["solar"] = ensure_block(
    "DA_Block_Solar", "solar_small", "Solar Collector", CUBE,
    [make_stage([make_entry(items["silicon_wafer"], 1), make_entry(items["plate"], 1)], 2.0)],
    module=module_class("SolarModule"), mass=18, health=120, power=1200.0)

# Attitude gyro: a 50 cm reaction-wheel triad (2x2x2 cells). 2000 N*m on a
# ~3 t rover is roughly 0.6-0.7 rad/s^2 in yaw and enough to right a hop -
# usable authority, far short of the old free-attitude magic. Orientation is
# irrelevant (the rating is isotropic), so it stays on the four-yaw aim list.
#
# gyro_momentum_capacity_nms is the rotor's own I*omega, NOT rating x seconds.
# Three rotors of about 18 kg at r = 0.2 m give I = 0.36 kg*m^2 each; at 2000
# rad/s (a 400 m/s rim, already at the limit of what a real rotor survives)
# that is 720 N*m*s, so 800 is the honest ceiling for what fits in a 0.5 m,
# 180 kg box. The old "rating x 5 s" arithmetic asked for 10,000 N*m*s per
# axis, which needs a 250 kg rotor storing 10 MJ - more mass in one rotor than
# the whole block, three times over - and that oversized store is what made the
# -w x h term the dominant torque in the vehicle and the attitude diverge.
#
# The control constants ride here because the attitude computer ships inside
# the block. The flight loop is a RATE command with a rate null on release -
# no attitude hold - and its one gain is DERIVED at runtime from the hull's
# measured inertia tensor: Kd = (2*zeta/T - hull damping)*I.
#
# attitude_settle_time_seconds 0.25 s (wn = 4 rad/s) gives Kd = 7.85*I, so the
# rate loop has a 0.127 s time constant: let go and the craft stops rotating in
# about an eighth of a second. 0.7 s was tried and it was the mushy roll - it
# put only 835 N*m per rad/s on the axis A/D drives, a ninth of what the pilot
# had before. The discrete rate-loop gain dt*Kd/I is 0.131 at 60 fps, 0.393 at
# 20 fps and 0.785 at 10 fps against a stability limit of 2, so the crisper
# value is still stable well below any playable frame rate. If roll ever feels
# twitchy, move this toward 0.35 s before touching a damping value.
#
# attitude_command_rate_ceiling_deg_per_sec 20 and attitude_sustained_turn_
# seconds 4.0 are ONE decision, because the rate limit now budgets the cost of
# HOLDING a rate as well as reaching it. Reaching w costs I*w of rotor
# momentum; holding it against the hull's 0.15 angular damping costs a further
# D*I*w EVERY SECOND. Budgeting only the spin-up said yaw could hold 30 deg/s,
# and the truth was that one continuous 202 degree turn - one mouse hold -
# filled the rotor and killed the axis. The limit is now
#     Fraction * Capacity / (I * (1 + D * T))
# so the budget covers the spin-up plus T seconds out of the rotors alone.
# At T = 4 s the band is 20.0 / 20.0 / 18.6 deg/s across roll, pitch and yaw,
# which is tight enough that one response models all three; 60 deg/s gave the
# pilot three different craft to fly at once. Past T the hold torque is carried
# by differential thrust (see the nozzle cant on the thruster block), so T is
# the guarantee for a craft with NO yaw geometry, not a limit on turn length.
#
# attitude_offload_time_constant_seconds 1.5 is the lag that splits the
# attitude command into a transient the rotors pay for and give back, and a
# sustained part thrust holds for ever. At 1.5 s a quarter-second stick input
# passes about 15 percent to the valves and a multi-second hold passes all of
# it, so "attitude transients never move a valve" survives - and the offload is
# force-nulled, so nothing it does moves the altitude or the ground track.
#
# attitude_bank_ceiling_deg 30 is a FLIGHT-ENVELOPE LIMITER on the attitude
# reference, and it is what stops A/D being a dive key. A/D is full-deflection
# roll, and the reference had no absolute bound at all - only a 5.0 degree leash
# to the hull, which is anti-windup and says nothing about where the hull ends
# up. Measured on the shipped rover from 200 m: 3 s of one held key reached 53.9
# degrees of bank, 5 s reached 93.2 degrees and 14.5 m of altitude, and past 90
# the six lift nozzles were pointing 21.3 kN at the ground - 1.2 g on top of
# weight, so 2.2 g of downward acceleration. Ten seconds reached 168.7 degrees,
# cost 288 m and -104 m/s against a landing-damage threshold of 8 m/s. Bounded
# at 30 degrees, the same 10 s of held D costs 0.1 m.
#
# The number is chosen against the craft rather than by taste. The reserved
# ceiling holds weight out to acos(18120 / (23738 x 0.90)) = 32.0 degrees, so a
# 30 degree ceiling keeps every commandable attitude one the rover can hold its
# own weight at - the lift governor can therefore never be PINNED by the stick
# alone - while still buying g tan(30) = 5.7 m/s^2 of lateral acceleration,
# which is how a thruster craft translates. It applies no torque of its own and
# does not stop a collision, a slope or a hard landing putting the hull outside
# it; the reference then re-seeds from the hull and the pilot flies it back. A
# craft with a worse thrust-to-weight ratio holds weight at a smaller angle and
# CAN still be flown into the pinned band, and the visor says PIN when it is.
#
# attitude_level_rate_deg_per_sec 20 is the pilot's way back to level, and it
# is deliberately the same number as the command ceiling: releasing the stick
# unwinds a bank exactly as fast as the stick put it in. A thrust vehicle has
# no restoring moment about its own centre of mass, so an arbitrary bank is a
# PERMANENT sideways acceleration - 20 degrees is 3.6 m/s^2, 45 degrees is
# 8.3 m/s^2, decaying only into linear damping for a 69 m/s drift. Nothing but
# the attitude system can end that. Roll and pitch are held; yaw is not, and a
# reference that does not exist on the ground cannot wind up against the
# suspension and release the moment the wheels leave it.
#
# momentum_dump_release_fraction 0.4 is hysteresis on the dump. Without it an
# unwind stopped the instant saturation fell back under the 0.8 onset and the
# axis PARKED there: 30 s of continuous pitch left the store at 80 percent for
# the rest of the flight, which is 45 percent of the commanded rate in one
# direction and not the fresh envelope a green readout implies.
#
# momentum_dump_onset_fraction 0.8 rather than 0.2. The onset MUST sit above
# attitude_command_momentum_fraction: at 0.2 the onset was 320 N*m*s while
# reaching the pitch or yaw rate limit alone costs 710 to 800, so the dump ran
# during essentially every manoeuvre - lift thrusters in constant motion as the
# normal state instead of a near-saturation recovery. Exoneer.Attitude.
# ConstantInvariants asserts the ordering so the two cannot be authored apart.
#
# momentum_ground_bleed_per_sec 0.25 is the always-available sink: the ground
# really does supply the external torque a reaction wheel needs to unwind. The
# gate is external SUPPORT, not tyre compression - a hull resting on the ground
# reacts the same torque a tyre does - because a craft built with no wheel
# blocks has an empty wheel array and used to have no sink at all. The rate is
# authored, not derived; it wants a derivation when the terramechanics work
# next opens.
#
# attitude_ground_release_seconds 0.25 debounces the ground/air decision on
# BOTH edges: airborne only after a quarter second clear, grounded only after a
# quarter second in contact. One edge was debounced and the other was not, so a
# single wheel tap on rough ground flipped the state instantly. What it gates
# is the LIFT GOVERNOR - hover hold must not open the valve on a craft sitting
# on its wheels, and one wheel tap must not slam it shut mid-flight. The
# attitude loop is deliberately not gated on contact: the triad rate-nulls
# whenever a pilot is aboard, on the ground and in the air.
blocks["gyro"] = ensure_block(
    "DA_Block_Gyro", "gyro_triad", "Attitude Gyro", CUBE,
    [make_stage([make_entry(items["motor"], 2), make_entry(items["computer_board"], 2),
                 make_entry(items["plate"], 2)], 4.0)],
    module=module_class("GyroModule"), mass=180, health=260, power=-450.0,
    size_in_cells=(2, 2, 2),
    extra={"max_gyro_torque_nm": 2000.0,
           "gyro_momentum_capacity_nms": 800.0,
           "attitude_settle_time_seconds": 0.25,
           "attitude_damping_ratio": 1.0,
           "attitude_command_rate_ceiling_deg_per_sec": 20.0,
           "attitude_command_momentum_fraction": 0.5,
           "attitude_sustained_turn_seconds": 4.0,
           "attitude_offload_time_constant_seconds": 1.5,
           "attitude_level_rate_deg_per_sec": 20.0,
           "attitude_bank_ceiling_deg": 30.0,
           "momentum_dump_rate_per_sec": 0.35,
           "momentum_dump_onset_fraction": 0.8,
           "momentum_dump_release_fraction": 0.4,
           "momentum_ground_bleed_per_sec": 0.25,
           "attitude_ground_release_seconds": 0.25})

blocks["fuel"] = ensure_block(
    "DA_Block_FuelTank", "fuel_tank", "Fuel Tank", CUBE,
    [make_stage([make_entry(items["plate"], 2), make_entry(items["motor"], 1)], 3.0)],
    module=module_class("FuelTankModule"), mass=80, health=220, power=0.0, storage=0.0,
    extra={"fuel_capacity_kg": 200.0})

# Wheels: 3x1x3 cells (75 cm block housing a ~70-84 cm tire with clearance).
# FOUR TERRAIN FAMILIES, distinguished only by real physical variables - width,
# operating pressure and how much of the soil's shear strength the tread can
# mobilise. Wong's critical ground pressure on the authored soils is roughly
# 65-130 kPa at rover wheel loads, so a 220 kPa road tire is RIGID everywhere
# (it digs) while a 45 kPa balloon tire is FLEXIBLE (it floats). That threshold
# is the whole reason terrain-specific wheels matter; nothing here is a bonus.
#
# Two interfaces, two numbers. On SOIL the tread mobilises a fraction of the
# soil's own shear strength (grouser effect), so lugs help and can exceed 1. On
# a HARD surface there is no soil to shear: the hit material's friction already
# is the rubber-on-surface coefficient, and what matters is how much of the
# contact patch actually lies on the surface - so the ordering reverses, and a
# smooth road tread beats deep lugs on rock. Neither number is a bonus; they
# describe different contacts, which is why the same wheel cannot win both.
#
# (radius m, width m, pressure kPa, soil tread mobilisation, hard-surface grip, mass kg)
#   road    narrow + hard   : cheap starter, best on the slab, digs in soft soil
#   sand    very wide + soft: flotation on dune sand, smooth tread
#   mud     lugged          : lugs shear soil against soil, so >1 - grips clay/mud,
#                             and stand the carcass off rock, so worst on hard ground
#   snow    widest + softest: maximum flotation on low-cohesion snow
wheel_families = [
    ("Road",  "road",  "Road Wheel",       0.35, 0.18, 220.0, 0.70, 1.00, 60),
    ("Sand",  "sand",  "Sand Balloon Wheel", 0.40, 0.40, 45.0, 0.75, 0.85, 75),
    ("Mud",   "mud",   "Lugged Mud Wheel",  0.38, 0.24, 120.0, 1.15, 0.72, 85),
    ("Snow",  "snow",  "Snow Flotation Wheel", 0.42, 0.50, 30.0, 0.95, 0.80, 95),
]

# UE pythonizes runs of capitals inconsistently ("KPa" can become "k_pa" or
# "kpa"), so try the plausible spellings and fail loudly rather than silently
# leaving an authored value at its C++ default.
def set_prop_any(target, names, value):
    for name in names:
        try:
            target.set_editor_property(name, value)
            return name
        except Exception:
            continue
    raise RuntimeError("None of %s exist on %s" % (names, target))

def make_wheel_spec(steerable, radius, width, pressure, tread, hard_grip):
    spec = unreal.VehicleWheelSpec()
    spec.set_editor_property("steerable", steerable)
    spec.set_editor_property("radius_m", radius)
    spec.set_editor_property("width_m", width)
    set_prop_any(spec, ["nominal_tire_pressure_k_pa", "nominal_tire_pressure_kpa"], pressure)
    spec.set_editor_property("tread_mobilisation", tread)
    spec.set_editor_property("hard_surface_grip", hard_grip)
    return spec

def make_wheel_mesh_transform():
    rotator = unreal.Rotator()
    rotator.set_editor_property("roll", 90.0)
    transform = unreal.Transform()
    transform.set_editor_property("rotation", rotator.quaternion())
    return transform

wheel_stage = [make_stage([make_entry(items["tire"], 1), make_entry(items["motor"], 1), make_entry(items["plate"], 1)], 3.5)]
for suffix, ident, display, radius, width, pressure, tread, hard_grip, mass in wheel_families:
    for steerable, role in ((True, "Steer"), (False, "Drive")):
        key = "wheel_%s_%s" % (ident, role.lower())
        blocks[key] = ensure_block(
            "DA_Block_Wheel%s%s" % (suffix, role), "wheel_%s_%s" % (ident, role.lower()),
            "%s (%s)" % (display, role), CYLINDER,
            wheel_stage, module=module_class("WheelModule"),
            mass=mass, health=250, power=-4000.0,
            size_in_cells=(3, 1, 3),
            extra={
                "is_wheel": True,
                # Explicit rather than leaning on ReplacePartAt's wheel-only
                # 'tire' fallback: every replaceable part names its spare.
                "spare_item_id": "tire",
                "wheel_spec": make_wheel_spec(steerable, radius, width, pressure, tread, hard_grip),
                "allow_terrain_overlap_on_place": True,
                "mesh_relative_transform": make_wheel_mesh_transform(),
            })

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
    ("recipe_tire", "Mold Tire", STATION.FABRICATOR, [("carbon", 4), ("iron_ingot", 1)], [("tire", 1)], 6.0, 250.0),
    ("recipe_fuel", "Refine Propellant", STATION.REFINERY, [("carbon", 2), ("ice", 1)], [("fuel", 4)], 5.0, 200.0),
    ("recipe_battery_cell", "Assemble Battery Cell", STATION.FABRICATOR,
     [("plate", 1), ("silicon_wafer", 1), ("carbon", 2)], [("battery_cell", 1)], 5.0, 200.0),
    ("recipe_seal_kit", "Assemble Seal Kit", STATION.FABRICATOR,
     [("carbon", 2), ("iron_ingot", 1)], [("seal_kit", 1)], 4.0, 150.0),
    ("recipe_suit_seal", "Assemble Suit Seal", STATION.FABRICATOR,
     [("carbon", 3), ("plate", 1)], [("suit_seal", 1)], 6.0, 200.0),
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
# 6.5 Soil physical materials + ground materials (wheel terramechanics)
# ---------------------------------------------------------------------------
log("--- soils ---")

# Physical materials are not data assets; they need their own factory.
def ensure_phys_material(name, klass):
    full = DIR_PHYSMATS + "/" + name
    if eal.does_asset_exist(full):
        return unreal.load_asset(full)
    factory = unreal.PhysicalMaterialFactoryNew()
    factory.set_editor_property("physical_material_class", klass)
    asset = asset_tools.create_asset(name, DIR_PHYSMATS, klass, factory)
    if asset is None:
        raise RuntimeError("Failed to create physical material " + full)
    log("created " + full)
    return asset

# Plain UObject-side class, resolved through reflection like module_class.
soil_class = unreal.load_class(None, "/Script/Exoneer.ExoneerSoilPhysicalMaterial")
if soil_class is None:
    raise RuntimeError("ExoneerSoilPhysicalMaterial class not found - build the C++ module first")

# Wong's published terrain values (docs/design/wheels/design-math-spec.md
# section 2). Authored in kN/kPa/deg; C++ ToSoilParams() converts to SI once.
pm_sand = ensure_phys_material("PM_Soil_DrySand", soil_class)
set_props(pm_sand, {
    "soil_display_name": unreal.Text("Dry Sand"),
    "bekker_kc": 0.99, "bekker_kphi": 1528.43, "bekker_n": 1.1, "bekker_n1": 0.9,
    "cohesion_kpa": 1.04, "friction_angle_deg": 28.0,
    "shear_deformation_k": 0.025, "shear_deformation_ky": 0.030,
    "unit_weight_kn_per_m3": 15.7,
})
pm_clay = ensure_phys_material("PM_Soil_Clay", soil_class)
set_props(pm_clay, {
    "soil_display_name": unreal.Text("Clayey Soil"),
    "bekker_kc": 13.19, "bekker_kphi": 692.15, "bekker_n": 0.5, "bekker_n1": 0.4,
    "cohesion_kpa": 4.14, "friction_angle_deg": 13.0,
    "shear_deformation_k": 0.010, "shear_deformation_ky": 0.012,
    "unit_weight_kn_per_m3": 16.8,
})
# Noted, not authored yet: sandy loam (5.27/1515.04/0.7/1.72 kPa/29 deg/0.025)
# and snow (4.37/196.72/1.6/1.03 kPa/19.7 deg/0.04). Surfaces without a soil
# physmat resolve as near-rigid ground in code; the tundra biome deliberately
# sets no DefaultSoil so the ground slab stays firm.

# Ground materials: the PhysMaterial rides on the material, not the mesh, and
# only counts when the instance also sets bOverridePhysMaterial.
BASE_MAT = unreal.load_asset("/Engine/BasicShapes/BasicShapeMaterial")

def ensure_ground_mic(name, color, phys_mat):
    full = DIR_MATS + "/" + name
    if eal.does_asset_exist(full):
        mic = unreal.load_asset(full)
    else:
        # The 5.8 factory exposes no initial_parent property; parent through
        # MaterialEditingLibrary after creation instead (idempotent re-apply).
        factory = unreal.MaterialInstanceConstantFactoryNew()
        mic = asset_tools.create_asset(name, DIR_MATS, unreal.MaterialInstanceConstant, factory)
        if mic is None:
            raise RuntimeError("Failed to create material instance " + full)
        log("created " + full)
    unreal.MaterialEditingLibrary.set_material_instance_parent(mic, BASE_MAT)
    unreal.MaterialEditingLibrary.set_material_instance_vector_parameter_value(
        mic, "Color", unreal.LinearColor(*color))
    mic.set_editor_property("phys_material", phys_mat)
    mic.set_editor_property("override_phys_material", True)
    return mic

mi_sand = ensure_ground_mic("MI_Ground_DrySand", (0.76, 0.65, 0.42, 1.0), pm_sand)
mi_clay = ensure_ground_mic("MI_Ground_Clay", (0.45, 0.28, 0.20, 1.0), pm_clay)

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
    pieces["floor"], pieces["ramp"], pieces["beam"], pieces["road"],
    pieces["road_light"], pieces["bridge_span"],
    pieces["shelter"], pieces["radio"], pieces["dish"], pieces["umbilical"],
    blocks["frame"], blocks["cockpit"], blocks["thruster"], blocks["gyro"],
    blocks["battery"], blocks["solar"], blocks["fuel"],
    blocks["wheel_road_steer"], blocks["wheel_road_drive"],
    blocks["wheel_sand_steer"], blocks["wheel_sand_drive"],
    blocks["wheel_mud_steer"], blocks["wheel_mud_drive"],
    blocks["wheel_snow_steer"], blocks["wheel_snow_drive"],
])
char_cdo.set_editor_property("starter_items", [
    make_entry(items["iron_ingot"], 40),
    make_entry(items["plate"], 30),
    make_entry(items["motor"], 12),
    make_entry(items["silicon_wafer"], 10),
    make_entry(items["computer_board"], 10),
    make_entry(items["ice"], 20),
    make_entry(items["tire"], 6),
    make_entry(items["fuel"], 20),
])
# Suit units moved from 0-100 bars to kJ / litres. Force the CDO so a BP that
# serialized the old 0.5 mining drain and 0.1 weld drain cannot keep them as
# kJ/s (which would make tools almost free on an 1800 kJ bank).
survival = char_cdo.get_editor_property("survival")
if survival:
    # CAPACITIES ONLY. Oxygen and SuitPower are the replicated CURRENT values
    # and are VisibleInstanceOnly, so they cannot be set on a template at all -
    # trying threw and aborted the whole script before the final
    # save_dirty_packages, which meant every authored constant above this line
    # was written to memory and never persisted. They are also meaningless as
    # defaults: the component fills them from the capacities at BeginPlay.
    survival.set_editor_property("suit_o2_capacity_l", 100.0)
    survival.set_editor_property("metabolic_o2_lps", 0.05)
    survival.set_editor_property("suit_power_capacity_kj", 1800.0)
    survival.set_editor_property("suit_power_drain_w", 540.0)
mining = char_cdo.get_editor_property("mining_tool")
if mining:
    mining.set_editor_property("suit_power_drain_per_sec", 9.0)
build_tool = char_cdo.get_editor_property("build_tool")
if build_tool:
    build_tool.set_editor_property("suit_power_per_weld_point", 1.8)

bp_gm = ensure_blueprint("BP_ExoneerGameMode", unreal.ExoneerGameMode)
gm_class = eal.load_blueprint_class(DIR_BP + "/BP_ExoneerGameMode")
gm_cdo = unreal.get_default_object(gm_class)
gm_cdo.set_editor_property("default_pawn_class", char_class)
gm_cdo.set_editor_property("player_controller_class", unreal.FirstPersonEngineerController.static_class())
gm_cdo.set_editor_property("hud_class", unreal.ExoneerHUD.static_class())
gm_cdo.set_editor_property("game_state_class", unreal.ExoneerGameState.static_class())

# ---------------------------------------------------------------------------
# 7b. Optional projects
# ---------------------------------------------------------------------------
log("--- projects ---")

def ensure_project(name, project_id, display, brief, criteria, duration_sols=0, grants_orbit=False):
    asset = ensure_data_asset(name, DIR_PROJECTS, unreal.ProjectDefinitionDataAsset)
    crits = []
    for typ, target in criteria:
        c = unreal.ProjectCriterion()
        c.set_editor_property("type", typ)
        c.set_editor_property("target", target)
        crits.append(c)
    set_props(asset, {
        "project_id": project_id,
        "display_name": unreal.Text(display),
        "brief": unreal.Text(brief),
        "criteria": crits,
        "duration_sols": duration_sols,
        "grants_orbital_knowledge": grants_orbit,
    })
    return asset

CRIT = unreal.ProjectCriterionType
ensure_project(
    "DA_Project_LongWatch", "long_watch", "The Long Watch",
    "Prove the settlement for seven sols, including a storm.",
    [(CRIT.POWER_RESERVE_HOURS, 0.05), (CRIT.OXYGEN_RESERVE_HOURS, 0.2),
     (CRIT.COMMS_HOPS, 1.0), (CRIT.STORM_SURVIVED, 1.0)],
    duration_sols=7)
ensure_project(
    "DA_Project_Handshake", "handshake", "The Handshake",
    "Haul and weld a dish. Hold it through a window. Yields orbital data.",
    [(CRIT.DISH_COMPLETE, 1.0), (CRIT.COMMS_HOPS, 1.0)],
    grants_orbit=True)
ensure_project(
    "DA_Project_Orbit", "road_to_orbit", "Road to Orbit",
    "Optional. The rocket is the last vehicle in a chain you built.",
    [(CRIT.PAD_POWERED, 400.0), (CRIT.FUEL_MASS_KG, 50.0), (CRIT.ASCENT_TWR, 1.2)])

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

    # Ground slab: 400 m x 400 m (engine cube is 100 uu), top surface at Z = -2.
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
# 8.5 Wheel test range - unconditional ensure-actor pass
# ---------------------------------------------------------------------------
# The branch above is create-only: an existing L_StarterPlanet is loaded, never
# updated. This pass runs on EVERY bootstrap, finds actors by label, spawns the
# missing ones, and re-applies transforms/materials - so tuning here reaches
# maps that already exist. Geometry facts: slab top is at Z = -2; the engine
# cube is 100 uu; "top flush at -2" means center Z = -2 - 50 * scale_z.
log("--- wheel test range ---")

def ensure_actor(label, cls, loc, rot=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0)):
    found = None
    for existing in eas.get_all_level_actors():
        if existing.get_actor_label() == label:
            found = existing
            break
    rotator = unreal.Rotator(roll=rot[0], pitch=rot[1], yaw=rot[2])
    if found is None:
        found = eas.spawn_actor_from_class(cls, unreal.Vector(*loc), rotator)
        found.set_actor_label(label)
        log("spawned " + label)
    found.set_actor_location(unreal.Vector(*loc), False, False)
    found.set_actor_rotation(rotator, False)
    found.set_actor_scale3d(unreal.Vector(*scale))
    return found

def ensure_test_slab(label, loc, rot, scale, mic=None):
    actor = ensure_actor(label, unreal.StaticMeshActor, loc, rot, scale)
    mesh_comp = actor.get_editor_property("static_mesh_component")
    mesh_comp.set_editor_property("static_mesh", CUBE)
    if mic is not None:
        mesh_comp.set_material(0, mic)
    return actor

# Sand field: the main sinkage/CTIS arena (spawn to its west edge is ~20 m of
# hard slab - the baseline drive). Clay field: the low-phi traction arena.
# Centers at -26, not -27: tops sit 1 cm proud of the slab instead of
# coplanar (coplanar z-fights and flickers).
ensure_test_slab("TestField_Sand", (6000, 2000, -26), (0, 0, 0), (80, 60, 0.5), mi_sand)
ensure_test_slab("TestField_Clay", (6000, -4000, -26), (0, 0, 0), (80, 40, 0.5), mi_clay)

# Sand-surfaced ramps rising east off the sand field; lower edge buried a few
# uu below grade so there is no lip. Center Z derivation for scale (25,10,0.4):
# centerZ - 1250*sin(pitch) + 20*cos(pitch) = -6  =>  191 @ 10deg, 403 @ 20deg.
ensure_test_slab("TestRamp_10deg", (10500, 3500, 191), (0, 10, 0), (25, 10, 0.4), mi_sand)
ensure_test_slab("TestRamp_20deg", (10500, 500, 403), (0, 20, 0), (25, 10, 0.4), mi_sand)

# Curbed bay inside the sand field (the "pit": sinkage IS the depth - drive in
# through the open west side, bog at full throttle; the ~35 cm hard curbs make
# casual exits impossible, so recovery is managed slip + dropped tire pressure).
ensure_test_slab("TestPit_Curb_N", (4000, 4200, 8), (0, 0, 0), (14, 0.4, 0.5))
ensure_test_slab("TestPit_Curb_S", (4000, 2800, 8), (0, 0, 0), (14, 0.4, 0.5))
ensure_test_slab("TestPit_Curb_E", (4700, 3500, 8), (0, 0, 90), (14, 0.4, 0.5))

# Garage: a pad near spawn with a ready-built rover on it (spawned at game
# start by ATestRoverSpawner unless a construct already exists nearby) - the
# first drive needs zero editor work and zero in-game construction.
ensure_test_slab("GaragePad", (900, -500, -26), (0, 0, 0), (12, 8, 0.5))
spawner_class = unreal.load_class(None, "/Script/Exoneer.TestRoverSpawner")
if spawner_class is None:
    raise RuntimeError("TestRoverSpawner class not found - build the C++ module first")
ensure_actor("TestRoverSpawner", spawner_class, (900, -500, 0), (0, 0, 0))

# World-space navigation signs (no UI, scope module 8 stays clean).
def ensure_sign(label, text, loc, yaw=180.0, size=180.0):
    actor = ensure_actor(label, unreal.TextRenderActor, loc, (0, 0, yaw))
    try:
        comp = actor.get_editor_property("text_render")
        comp.set_editor_property("text", unreal.Text(text))
        comp.set_editor_property("world_size", size)
        comp.set_editor_property("text_render_color", unreal.Color(140, 230, 255, 255))
    except Exception as sign_error:
        log("sign '%s' skipped: %s" % (label, sign_error))
    return actor

ensure_sign("Sign_Garage", "GARAGE - PRESS F TO DRIVE", (900, -500, 320), yaw=180, size=120)
ensure_sign("Sign_Sand", "SAND FIELD", (2100, 2000, 320))
ensure_sign("Sign_Clay", "CLAY FIELD", (2100, -4000, 320))
ensure_sign("Sign_Ramps", "RAMPS 10 / 20 DEG", (9100, 2000, 420))
ensure_sign("Sign_CurbBay", "CURB BAY", (3200, 3500, 320))

les.save_current_level()
log("test range ensured")

# ---------------------------------------------------------------------------
# Save everything
# ---------------------------------------------------------------------------
unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
log("bootstrap complete: %d steps logged" % len(log_lines))
