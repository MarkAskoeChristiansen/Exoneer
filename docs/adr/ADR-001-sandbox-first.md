# ADR-001: Sandbox-first product architecture

- **Status**: accepted
- **Date**: 2026-09-03
- **Deciders**: Mark
- **Tags**: product, architecture, 1.0-scope

## Context

Exoneer was accumulating systems that could be read as a campaign with a
sandbox attached: projects, an escape rocket, talents, a second world.
GAME-SCOPE still wins on *how a system behaves*, but it does not say what
the shipped game *is*. Without a product rule, a later feature can lock
tools behind a project, invent a quest token, or treat Road to Orbit as a
cutscene.

The 2026-09-03 design contract (`docs/VISION.md`, `docs/design/sandbox.md`)
states the empty-cell thesis: honest construction + dirt + wear, playable
indefinitely, with an optional rocket whose difficulty is the colony the
player already built.

## Decision

**The sandbox is the game.** A player who never opens the wrist project
list is playing the whole product.

Binding rules:

1. Core verbs (mine, refine, fabricate, construct, drive, inspect, repair)
   work with no accepted project.
2. Projects are optional exams of systems the sandbox already has. They
   never grant tools, recipes, pieces, or map of the basin.
3. Road to Orbit is the unique endgoal and is optional. If the P4 ascent
   prototype misses the sim bar in `docs/ROADMAP.md`, 1.0 ships the sandbox
   without it — no scripted launch.
4. When a feature disagrees with *what the game is*, VISION wins. When it
   disagrees with *how a system must behave*, GAME-SCOPE wins.
5. Completeness is the nine-step test in `docs/design/sandbox.md` §1, not
   a campaign checklist.

## Consequences

### Positive

- New work has a reject test: "does the sandbox still play if the player
  never accepts a project?"
- Road to Orbit can slip a release without bricking 1.0.
- Co-op and save design stay session-first (1–4 listen-server), not
  mission-instance.

### Negative

- Campaign-shaped content (mandatory onboarding, tool unlocks, quest
  items) is out of 1.0 even if it would ship faster.
- Basin authorship and the mine→fabricate→operate→maintain loop become
  the critical path, not story beats.

### Neutral

- Long Watch, Handshake, and Road to Orbit still exist; they live under
  ADR-003 as data, not as the spine.
- Existing CVar `exoneer.Creative` remains a prototype on-ramp, not a
  product mode. Survival economy is the 1.0 default once the loop closes.

## Links

- Depends on: none (root product ADR)
- Related: [ADR-002](ADR-002-no-weld-heal.md), [ADR-003](ADR-003-project-subsystem.md)
- Spec: [VISION.md](../VISION.md), [design/sandbox.md](../design/sandbox.md)
- Scope: [GAME-SCOPE.md](../GAME-SCOPE.md)
- Matrix: [ROADMAP.md](../ROADMAP.md) V-SANDBOX
