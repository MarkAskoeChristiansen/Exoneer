# ADR-002: No weld-to-heal; Health and condition stay separate

- **Status**: accepted
- **Date**: 2026-09-03
- **Deciders**: Mark
- **Tags**: maintenance, build-tool, vehicles, 1.0-scope

## Context

v2 used weld-on-Complete as a generic heal: `UBuildToolComponent` spent
weld points into `ABasePiece::RepairHealth` / `AVehicleConstruct::RepairBlockAt`
and restored `Health`. That is a healing beam. It collapses two channels
GAME-SCOPE §10 forbids mixing:

| Channel | What it is | Restore |
|---|---|---|
| Immediate damage (`Health`) | Discrete event (impact, collapse) | Patch, replace, or scrap |
| Condition | Continuous physical state (tread mm, °C, fade, dust, leak) | Wipe, service, or **replace the part** |

A single 0–1 durability bar, pawn regen as maintenance, and welding a
Complete tire/battery/dusty panel to restore condition are banned.

Wheels are the first swappable class. Once they exist, the heal path must
die or the teaching case (hard drive spends tread; replace restores) is a
lie.

## Decision

**Weld is construction only.** Primary on a Complete part never restores
Health or condition.

Routing in `UBuildToolComponent::Server_Weld` (authority):

1. Ghost / incomplete → `InvestConstruction` (unchanged).
2. Complete + dusty solar / radio face → `WipeDust` / `WipePartAt`.
3. Complete + worn/failed swappable part + spare in inventory →
   `ReplacePartAt` (consume item, reset that record's condition).
4. Else → no-op feedback ("already complete"). No `RepairHealth`.

`ABasePiece::RepairHealth` and `AVehicleConstruct::RepairBlockAt` remain
as dead symbols that **return 0**. They will be deleted once callers and
the weak CDO test are gone (ROADMAP gap 7).
`RepairHealthPerWeldPoint` on the build tool is leftover and must leave
with them.

Condition lives on `FPartCondition` (SI fields, N/A as `< 0`), replicated
on the piece/block record, saved, and shown in mm / °C / kPa / kJ / L/s —
never "72%". Spend is server-only from the system that measures the cause
(slip×load, copper loss, storm exposure).

Legal verbs match the failure; the tool in the hand does not choose the
outcome.

## Consequences

### Positive

- V-NOWELDHEAL is a real contract: weld a Complete wheel, Health and tread
  are unchanged.
- Replace is the 1.0 restore for tires, motors, panels, batteries — the
  fabricator loop has a reason to exist.
- Diagnosis stays in engineering units (GAME-SCOPE §8 still bans arcade
  bars).

### Negative

- Immediate-damage patch (weldable metal crack) is deferred until a crack
  flag exists. Until then, damaged metal is replace or scrap.
- Dead heal declarations still compile; a CDO `RepairBlockAt` test is
  weaker than welding a live Complete wheel through the build tool.

### Neutral

- Tire tread and dust opacity already spend. Motor winding temperature is
  written, not consumed (ROADMAP gap 4). Battery fade, seal leak, and
  structure fatigue are not yet classes (gap 6).

## Links

- Depends on: [ADR-001](ADR-001-sandbox-first.md) (honest maintenance is
  a sandbox verb, not a quest)
- Related: [ADR-003](ADR-003-project-subsystem.md) (projects query
  physical readings, not durability bars)
- Spec: [design/maintenance.md](../design/maintenance.md)
- Rules: [GAME-SCOPE.md](../GAME-SCOPE.md) §10
- Seams: [ARCHITECTURE-V2.md](../ARCHITECTURE-V2.md) §16
- Matrix: [ROADMAP.md](../ROADMAP.md) V-NOWELDHEAL, V-TREAD
