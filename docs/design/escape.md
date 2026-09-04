# Road to Orbit (escape) specification

Parent: [VISION.md](../VISION.md). Project wrapper: [design/projects.md](projects.md).
The rocket is **the last vehicle** in a logistics chain the player already operates. If this cannot be simulated honestly, it slips a release. It never becomes a cutscene.

---

## 1. 1.0 gate

Ship Road to Orbit in 1.0 **only if** a grid-built ascent craft prototype demonstrates, in PIE, all of:

- Mass and CoM from welded blocks (existing `AVehicleConstruct`)
- Off-axis thrust produces rotation (existing thrusters + gyro)
- A **fuel** resource that depletes and can starve the engines
- Control authority that can fail (under-power, empty tank, missing gyro)
- A failure that is funny/honest (flip, brown-out, pad strike) — not a fade-to-black win

If that prototype misses the bar, 1.0 is the sandbox + Long Watch + Handshake. This spec stays intact for the next release.

**Out of 1.0 even if the gate passes:** full n-body orbital mechanics, staging minigames as UI, Earth-like atmosphere table, CFD. Reduced-order SI: `m * a = Σ F`, vacuum or a single exponential atmosphere, patched conic or even “reach apoapsis altitude X and circularize with remaining Δv” as the success query.

---

## 2. The logistics chain (all optional-project criteria, all physical)

To escape, the crew must actually:

| Step | Sandbox systems it reuses | Physical checks |
|---|---|---|
| 1. Survey a site | Relays, wrist, walking/driving | Slope ≤ limit; soil family not clay-bowl; altitude/sky fraction; access path exists |
| 2. Roads, bridges, winch points, depots, shelters | Socket pieces, support, storms | A loaded hauler can traverse site↔industry without collapsing a span; a shelter stands a storm |
| 3. Heavy haulers | Wheels, CTIS, cargo mass, wear | Axle load, tread, brake-hold on the grade with the **stack mass** on the bed |
| 4. Manufacture + transport every module | Refine, fabricate, cargo | Modules exist as blocks/pieces in inventories or on beds, not as spawned props |
| 5. Pad, tower, power, cooling, fuel | Structures, power network, (simple) fluid tanks | Pad complete; power snapshot covers pump/igniter; fuel tank stored ≥ burn |
| 6. Assemble + certify stack | Vehicle grid, construction stages | Stack is one construct (or stacked constructs with a documented clamp); CoM inside a stability envelope at ignition |
| 7. Weather delays, static fire, degradation | Environment, condition | Window vs storm; a static-fire criterion (hold thrust on pad T seconds without scrap); worn parts fail honestly |
| 8. Ascent; co-op GSE | Pilot input, player-owned RPCs | Pilot flies; partners may run pumps/umbilicals/hold-down as interactions on pad machines |

No step is a cutscene. Skipping Handshake means the crew guesses windows and may static-fire into a storm.

---

## 3. Ascent vehicle (bounded model)

Reuse `AVehicleConstruct`. Do not invent a second vehicle actor.

Additions (escape-gated, not required for rovers):

- `UFuelTankModule`: stored kg, feed rate, replicates `StateScalar` as fill fraction
- Thruster draw: existing power ledger **plus** fuel mass flow. No fuel → no thrust
- Hold-down clamps: pad-side pieces that restrain until a server command
- Fairing optional (mass). No aero-fancy; a single drag term vs mach/altitude is enough if atmosphere exists

Certification (wrist, physical):

- Dry mass, wet mass, CoM offset from thrust centroid
- TWR at pad (must exceed 1 + margin in local g)
- Control authority: gyro torque vs predicted max moment from engine-out
- Fuel mass ≥ integrated burn for the reduced-order ascent profile

Fail certification = you may still ignite (sandbox). The project criterion fails if you ignore it and explode.

---

## 4. Pad and ground support

Pad is a **base structure** (civil grammar) next to a **stack** (vehicle grammar). That meeting is the late-game of the two grammars — not a third builder.

Minimum machines: fuel pump (power + tank), igniter, hold-down, a shelter for GSE crew. Cooling may be a power+water placeholder until fluids exist; do not fake it with a “cooling %” buff. If fluids are not ready, the pad criterion uses **power + fuel mass + shelter** only, and cooling waits.

---

## 5. After escape

The construct that reaches the success altitude/orbit is not deleted as a reward screen.

Options, all persistent:

- **Depart:** that pawn leaves the basin save (spectate or sit out). Colony remains.
- **Return:** a capsule construct (or the same stack, reduced-order deorbit) can be flown back. Same `USaveGame` slot.
- **Leave crewmates:** other players keep the listen-server colony.

There is no “you won, new game+.”

---

## 6. Anti-patterns

- Menu “Launch” button
- A pre-built NASA rocket in a crate
- Success because the Handshake was completed (Handshake only gives data)
- Deleting the colony on liftoff
- Full KSP career tech tree
