# mod-mount-progression

A **per-mount XP / leveling system** for AzerothCore (WotLK 3.3.5a). Every mount a player
owns has its own level and XP, earned through activities that match the mount's type.

Full design lives in the core repo at `docs/MOUNT_PROGRESSION_SPEC.md`.

## Highlights

- Independent level + XP per owned mount, capped at `MountProgression.MaxLevel` (default 60).
- Quadratic XP curve scaled by mount rarity — `xp_to_next(level) = XPBase.<Rarity> * level²` —
  so higher rarities take longer to cap by design.
- XP awarded for activities matching the active mount (e.g. flat kill XP on creature kills).

## Configuration

See [`conf/mod_mount_progression.conf.dist`](conf/mod_mount_progression.conf.dist) for the
full, commented list. All numbers are tuning targets — edit, reload worldserver, tune.

| Setting | Default | Description |
| ------- | ------- | ----------- |
| `MountProgression.Enable` | `1` | Master switch. When off, no XP is awarded and commands report "disabled"; the catalog still loads. |
| `MountProgression.MaxLevel` | `60` | Level cap for every mount. |
| `MountProgression.XPBase.<Rarity>` | `10`–`4000` | Per-rarity XP-curve base (Common → Legendary). |

## Commands

| Command | Access | Description |
| ------- | ------ | ----------- |
| `.mount` | Player | Show info for the active mount. |
| `.mounts` | Player | List the player's mounts and their levels. |
| `.mount give <...>` | GM | Grant a mount. |
| `.mount addxp <...>` | GM | Add XP to a mount. |
| `.mount setlevel <...>` | GM | Set a mount's level. |

## Installation

See the [repository README](../README.md) for how to junction/symlink this module into
your AzerothCore `modules/` directory, then re-run CMake and rebuild.
