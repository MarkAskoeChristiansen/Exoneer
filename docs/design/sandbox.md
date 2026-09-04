# Sandbox specification

Parent: [VISION.md](../VISION.md). Behavior rules: [GAME-SCOPE.md](../GAME-SCOPE.md).
This file is the contract for **unrestricted play**. If a player never accepts a project, everything here still has to work.

---

## 1. Completeness test

A stranger, no wiki, no accepted project, can in one session:

1. Spawn at the crash-pod.
2. Mine at least two deposits.
3. Ghost a foundation, wall, floor, roof; weld them; the hut stands.
4. Place solar + battery, weld, wait for stored energy.
5. Enqueue a refinery and fabricator recipe; take plates/motors from output.
6. Found a rover, add cockpit + battery + wheels, weld, enter, drive onto **firm, sand, and clay**.
7. Notice a physical reading change (temperature, slip, or tread) after hard driving.
8. Recharge suit power at a station (umbilical, not a radius cheat once that actor exists).
9. Save, reload, the hut and rover are still there.

Fail any step and the sandbox is not 1.0.

A second player joining mid-session can weld the same ghost and sit in a second seat or walk the hut. Fail that and co-op is not 1.0.

---

## 2. Economy loop (mine → fabricate → operate → maintain)

```
deposit  --mine-->  ore
ore      --refinery, power-->  ingot / wafer
ingot    --fabricator, power-->  plate, motor, board, tire, spare
spare    --weld ghost / replace part-->  structure or vehicle
operate  --use, overload, weather-->  condition spend + maybe immediate damage
inspect  --visor / wrist / dash-->  physical readings
repair   --legal verb for that failure-->  restored function
```

Rules:

- Ghosts are free. Completing them consumes fabricated parts and weld-work (existing `UConstructionComponent` / block stages).
- Machines only run when construction is Complete and `SupplyFraction` allows. Under-power slows; it does not invent resources.
- Spares are items. You cannot "repair" a wheel without a tire/motor in inventory.
- There is no vendor. Wrecks are salvage, not shops.
- Conveyor radius-cheat (`UConveyorComponent` 600 uu) is **prototype only**. 1.0 economy must work by walking items or cargo beds. Do not design factories around the radius.

---

## 3. Construction and civil sandbox

Must be placeable without a project:

| Kit | Role |
|---|---|
| Foundation, wall, floor, ramp, roof, beam, door frame | Hut |
| Solar, battery, refinery, fabricator, O2 generator | Industry / life support |
| Frame, cockpit, battery, solar, thruster, gyro, wheel families (road, sand, mud, snow) | Vehicles |
| Road deck, short bridge, storm shelter | Logistics (new pieces; same socket grammar) |

Rules already in C++ that this spec depends on:

- First grounded foundation founds `ABaseStructure`.
- Support-budget solver: unsupported pieces collapse. Enough for 1.0 "no floating." Vector-load FEM is post-1.0.
- Ghosts: no pawn collision, no support, no power.
- Storms damage **complete exposed** pieces (existing manager). Condition spend (dust, fatigue) is [maintenance.md](maintenance.md).

Roads and bridges: socket-snapped pieces that **carry vehicle mass**. 1.0 may use the support budget plus a "deck" mount tag. A loaded rover on a deck with SupportValue ≤ 0 collapses the span. Wind on a shelter is a storm-resistance check plus condition spend, not a new solver in the first prototype.

---

## 4. Driving sandbox

1.0 vehicle must already do, or finish:

- Suspension, steering (Ackermann), service brake, handbrake, wheel-family swap (road, sand, mud, snow)
- Cargo mass on the bed changes CoM and per-wheel `W`
- Soil feedback: firm / sand / clay via `UExoneerSoilPhysicalMaterial`
- Recovery: winch is post-prototype; 1.0 minimum is "can reverse out, can swap a wheel, can ghost a new rover"
- Split detection on block removal (exists)

Playable exam on the authored basin (no project):

- Hard slab: holds brake on a 10° pad, steers, does not bog
- Dry sand: road tires dig; sand or snow family floats
- Clay: slippery, shear-limited; raw throttle is the wrong lesson

---

## 5. Basin design (one map, several soils)

`L_StarterPlanet` is the 1.0 world. Authored, not PCG.

Required spatial facts (names are working):

| Feature | Why it exists |
|---|---|
| Crash-pod bowl (firm-ish pad) | Onboarding, first hut |
| Clay cut | Shear / slip lesson |
| Sand bowl | Sinkage / wheel-family lesson |
| Grade ~10° | Brake hold + haul |
| Wash / gap | Wants a bridge (or a long drive around) |
| Ridge road | Wants a shelter and a dish site (Handshake uses it; sandbox uses it as a view) |
| Two wrecks | Salvage + logs (history, not quest NPCs) |
| Flora kit | Anti-SE-grey; marks soil; may foul tread (maintenance) |

No fauna. No second planet. Sky may show other bodies.

---

## 6. Persistence and co-op

- Server-only save (`USaveGameSubsystem`). Load respawns through placement APIs.
- 1.0 save must include: player, inventories, structures, vehicles, environment time/storm, **condition readings**, **project runtime** (even if none accepted).
- Join-in-progress: replication already rebuilds pieces and block fast-arrays. Condition and projects must ride the same channels (ARCHITECTURE-V2 §§16–18).
- A save that ignored every project is a valid 1.0 save.

---

## 7. Onboarding (blind test)

No mission pop-up. The crash-pod, a wreck with a readable log, and the visor are the tutorial.

Blind-test fail if the player cannot discover: mine, Q-cycle tools, B/ghost, weld, E-interact machine, F-cockpit, V mode toggle, Z brake.

Wrist computer is look-at-forearm (GAME-SCOPE §8). Prototype may use a key that *simulates* that until the mesh exists; the key must not be a quest log.

---

## 8. Out of sandbox scope

Talents, EOS, second biome, fauna, guns, hunger, belt empires, orbital menu travel.
