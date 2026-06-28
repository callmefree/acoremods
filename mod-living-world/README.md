# mod-living-world

Foundational state graph module for the Living World Stormwind prototype.

**Status:** Phase 1 — foundation only.

Phase 1 ships per-guild flag storage with cache, persistence, and (slice 2) GM commands. Phases 2–5+ are designed in `docs/living_world_state_graph_design.md` but not built.

## Documentation

- [Operational spec](../../docs/living_world_prototype.md) — what the prototype is testing, phase gates, kill criteria
- [Inspirations and gap analysis](../../docs/living_world_inspirations.md) — the depth model
- [State graph design](../../docs/living_world_state_graph_design.md) — schema, API, actor-type tiers
- [Phase 0 reconnaissance](../../docs/phase_0_recon.md) — fork, hooks, removal story

## Removal

See `docs/living_world_state_graph_design.md` §13. Drop two tables, delete pending SQL files, remove this directory, rebuild.
