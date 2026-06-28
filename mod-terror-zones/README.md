# mod-terror-zones

Wall-clock-driven rotation of **"empowered" open-world zones** for AzerothCore
(WotLK 3.3.5a) — inspired by Diablo-style terror zones.

Full design lives in the core repo at `docs/TERROR_ZONES_SPEC.md`.

## How it works

- A selection loop picks `SlotCount` zones to empower each rotation tick.
- Ticks fire at **wall-clock-aligned** boundaries: `tick_at = epoch - (epoch % interval)`,
  so an hourly rotation fires at `:00:00` every hour — not one hour after boot.
- Multi-slot picks deduplicate, so no zone is empowered twice in the same tick.
- Empowerment carries cosmetic flavor (weather/atmosphere) and, in later slices, scaling
  and rewards (rewards integrate with [mod-custom-items](../mod-custom-items)).

## Configuration

See [`conf/mod_terror_zones.conf.dist`](conf/mod_terror_zones.conf.dist) for the full,
commented list. All numbers are tuning targets — edit, reload worldserver, tune.

| Setting | Default | Description |
| ------- | ------- | ----------- |
| `TerrorZones.Enable` | `1` | Master switch. When off, no rotation is selected and commands report "disabled"; the pool still loads. |
| `TerrorZones.Debug` | `0` | Extra debug logging on the `module` log channel. |
| `TerrorZones.RotationIntervalSeconds` | `3600` | Rotation cadence (clamped to a 60s minimum). |
| `TerrorZones.SlotCount` | `1` | Zones empowered simultaneously; raise to 2–3 for larger servers. |

## Commands

| Command | Access | Description |
| ------- | ------ | ----------- |
| `.zones` | Player | Show the current empowered zone(s). |
| `.zones next` | Player | Time until / preview of the next rotation. |
| `.zones history` | Player | Recent rotation history. |
| `.zones announce` | Player | Manage announcement preferences. |
| `.zones tick` | GM | Force a rotation tick now. |
| `.zones pool` | GM | Show the zone selection pool. |
| `.zones settier <...>` | GM | Set the empowerment tier. |
| `.zones setflavor <...>` | GM | Set the empowerment flavor. |
| `.zones testweather \| testflavor \| testclear` | GM | Preview/clear weather & flavor effects. |
| `.zones event list \| fire <...> \| end` | GM | Manage dynamic world events. |

## Installation

See the [repository README](../README.md) for how to junction/symlink this module into
your AzerothCore `modules/` directory, then re-run CMake and rebuild.
