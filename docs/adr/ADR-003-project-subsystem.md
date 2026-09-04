# ADR-003: Optional projects via UProjectSubsystem and GameState

- **Status**: accepted
- **Date**: 2026-09-03
- **Deciders**: Mark
- **Tags**: projects, networking, persistence, 1.0-scope

## Context

VISION requires optional exams of the sandbox (Long Watch, Handshake, Road
to Orbit) that never grant tools. v2 had no place for that state: no
catalog, no replicated session list, no save field.

Two implementation shapes were on the table:

- Quest-style actors / GameMode flow (tokens, triggers, subclasses).
- A world subsystem that evaluates **physical queries** against live
  systems, with a compact replicated array on GameState for JIP.

World subsystems do not replicate. GameMode is a poor home for
session-shared optional state. Per-player quest components would fork
co-op (one player "on" the Handshake, another not) and invite item
rewards.

## Decision

**Projects are data + one server tick + a replicated GameState array.**

| Piece | Role |
|---|---|
| `UProjectDefinitionDataAsset` | Catalog. `PrimaryAssetType = "Project"`, scanned from `/Game/Exoneer/Data/Projects`. Criteria are `EProjectCriterionType` + SI target. `DurationSols` is the hold window. No item rewards. Handshake may set `bGrantsOrbitalKnowledge`. |
| `UProjectSubsystem` | `UTickableWorldSubsystem`. Server-only evaluate at 1 Hz. Accept / Abandon mutate GameState. Not a replication channel. |
| `AExoneerGameState` | Replicates `TArray<FProjectRuntime>` and `FOrbitalKnowledge`. Clients read this. |
| Player-owned Server RPC | Accept / Abandon on the character (constructs are not connection-owned). State is **shared** on the listen-server. |
| Save | `TArray<FProjectRuntime>` + orbital knowledge on `UExoneerSaveGame`. |

Evaluator reads existing systems (power snapshot, oxygen, storms, dish,
pad, fuel, TWR). It does not spawn tokens, lock recipes, or wipe the save
on fail. Failure logs a diegetic line (physical reason, units). Retry is
free.

Long Watch, Handshake, and Road to Orbit are three data assets, not three
C++ subclasses.

Shipped criterion types today: `PowerReserveHours`, `OxygenReserveHours`,
`CommsHops`, `StormSurvived`, `DishComplete`, `PadPowered`, `FuelMassKg`,
`AscentTwr`. **Not yet:** `DishOnAzimuth` and a window clock, so Handshake
cannot fail a window (ROADMAP V-HAND / gap 3). `DurationSols` is 0 on the
bootstrap Handshake def.

## Consequences

### Positive

- Ignoring projects does not brick the save (ADR-001).
- JIP sees the same Active/Failed list without a second channel.
- New projects are content (data asset + bootstrap), not a code fork.

### Negative

- Handshake is runnable but cannot fail until azimuth + window types and
  a non-zero `DurationSols` exist.
- Criterion enum growth is a code change; keep types physical, not
  "brought 10 iron".
- `ARCHITECTURE-V2.md` §§17–18 still say "not implemented" — the working
  tree is ahead of that header (doc debt).

### Neutral

- Wrist list is presentation only; visor never gets a quest marker.
- Two-client JIP of project state is untested (ROADMAP V-JIP / gap 9).

## Links

- Depends on: [ADR-001](ADR-001-sandbox-first.md)
- Related: [ADR-002](ADR-002-no-weld-heal.md) (criteria may read
  condition; they must not read a healable HP bar)
- Spec: [design/projects.md](../design/projects.md), [design/escape.md](../design/escape.md)
- Seams: [ARCHITECTURE-V2.md](../ARCHITECTURE-V2.md) §§17–18
- Matrix: [ROADMAP.md](../ROADMAP.md) V-WATCH, V-HAND, V-ORBIT, V-SAVE, V-JIP
- Code: `Source/Exoneer/World/ProjectSubsystem.*`, `ExoneerGameState.*`,
  `Data/ProjectDefinitionDataAsset.h`
