# Optional projects specification

Parent: [VISION.md](../VISION.md).
Projects are **optional exams** of the sandbox. They never grant tools, recipes, or pieces the sandbox does not already have.

---

## 1. Framework

### 1.1 Definition (data)

`UProjectDefinitionDataAsset` (`PrimaryAssetType = "Project"`):

| Field | Role |
|---|---|
| `ProjectId` | Stable name (`long_watch`, `handshake`, `road_to_orbit`) |
| `DisplayName`, `Brief` | Wrist copy |
| `Prerequisites` | Other project ids **or none**. Empty = always offerable |
| `Criteria` | Array of `FProjectCriterion` (see §1.3) |
| `FailureIsRetryable` | Always true for 1.0 projects |
| `Rewards` | **No items.** May unlock *knowledge*: ephemerides, log pages, a forecast channel |

Handshake’s only reward: **escape-specific orbital data** (windows, inclination, pad-siting constraints). Not a map of the basin. Not an ending.

### 1.2 Runtime (server)

`UProjectSubsystem` (world subsystem, server-authoritative):

- Replicated list of `FProjectRuntime { Id, EProjectState, StartedAtSol, LastFailCode, CriterionSnapshots }`
- States: `Available`, `Active`, `Succeeded`, `Failed`, `Abandoned`
- Any player on the listen-server may Accept / Abandon via a **player-owned** Server RPC (wrist). State is shared.
- Tick (slow, 1 Hz): evaluate criteria against the **world**, not against a quest inventory.

Join-in-progress and save/load must round-trip runtime (ARCHITECTURE-V2 §17).

### 1.3 Criteria are physical queries

`FProjectCriterion`:

```
Type: Enum (PowerReserve, OxygenReserve, CommsHops, StormSurvived,
            DishOnAzimuth, DishPowered, RelayLOS, SiteSlope,
            SiteSoil, PadComplete, StaticFire, ...)
Target: float or tag (SI units)
DurationSols: optional hold time
```

The evaluator reads existing systems (`UPowerNetworkComponent` snapshot, `UOxygenComponent`, environment storm flags, dish azimuth, support, condition). It does **not** spawn invisible tokens.

Failure logs a **diegetic** line on the wrist (physical reason, units). Retry is free.

---

## 2. The Long Watch

**Offer:** always, once a powered hut exists (heuristic: ≥1 complete solar + battery + any sealed or roofed volume). Still optional.

**Exam:** seven sols of self-sufficiency including **one severe storm**.

Must hold, simultaneously, for the window:

| Criterion | Physical meaning |
|---|---|
| Life support | Suit or habitat O2 and suit power never hit 0 for any living player |
| Reserves | Stored energy ≥ N hours of night demand; O2 ≥ N hours makeup |
| Communications | ≥1 powered radio/relay that can see the hut (even a local mast) |
| Local spares | Fabricator has produced ≥1 spare tire **or** solar **or** plate *during* the watch (not started-with debug items) |
| Storm | Named severe storm occurs during the seven sols; hut still standing after; power recovered without a debug cheat |

Fail: a log (“brown-out at 03:12”, “seal leak 1.2 L/s”). Colony continues. Retry.

This project exists to **validate the economy**, not to tell a story.

---

## 3. The Handshake

**Offer:** always. Not required for Long Watch or for sandbox. Useful for Road to Orbit.

**Exam:**

1. Survey/choose a ridge or high site (player judgment; criterion: line-of-sight to sky fraction).
2. Ghost, haul, weld a dish + mast + power.
3. Hold azimuth + power + at least one relay hop for a **system-net window** (clock on `APlanetEnvironmentManager`).
4. Storm or wind may yaw a worn actuator — condition matters.

**Reward:** orbital / window data written to the wrist (used by [escape.md](escape.md)). Basin map remains relay-known-terrain only.

**Not:** an ending, a credits roll, a second-planet unlock, a tool unlock.

Fail/retry: same as Long Watch. Next window.

---

## 4. Road to Orbit

Definition lives in [escape.md](escape.md). It is a project in this framework (`road_to_orbit`) with many criteria. It is **not** 1.0 unless the flight prototype passes the sim bar.

Prerequisite: none strictly. Handshake data makes siting and windows honest; a crew can attempt a dumb stack and fail physically.

---

## 5. Diegetic presentation

- Wrist: project list, criteria as **readings vs targets**, failure logbook.
- Visor: no quest marker, no compass objective.
- Radio: optional forecast / window broadcast once Handshake succeeded.
- Relays: terrain knowledge only where powered LOS exists (GAME-SCOPE §6).

---

## 6. What projects must never do

- Lock a recipe, piece, block, or tool
- Complete because the player walked into a trigger
- Consume a “quest item” that has no inventory existence
- Wipe the save on fail
- Run as a mandatory onboarding
