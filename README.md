# acoremods

A collection of custom [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) modules authored by jfalcos.

## Modules

| Module | Description |
| ------ | ----------- |
| `mod-custom-items` | Custom items with no client patch (donor-appearance framework). Requires [core patch](core-patches/). |
| `mod-dynamic-ah` | Dynamic auction house. |
| `mod-living-world` | Dynamic world / living-world behaviors. |
| `mod-mount-progression` | Per-mount XP / leveling. Requires [core patch](core-patches/). |
| `mod-terror-zones` | Rotating "terror" zones with empowered open-world content. Requires [core patch](core-patches/). |

## Installation

AzerothCore discovers modules as direct children of its `modules/` directory.
Each module in this repo must therefore appear at `azerothcore-wotlk/modules/<module-name>`.

### Step 1 — Place the modules

Pick one of the following:

#### Option A — Directory junctions (keep the monorepo separate, recommended on Windows)

Clone this repo anywhere, then junction each module into your AzerothCore `modules/` folder:

```powershell
$src = "<path-to>\azerothcore-wotlk\modules"
$dst = "<path-to>\acoremods"
foreach ($m in @("mod-custom-items","mod-dynamic-ah","mod-living-world","mod-mount-progression","mod-terror-zones")) {
  New-Item -ItemType Junction -Path (Join-Path $src $m) -Target (Join-Path $dst $m)
}
```

On Linux/macOS use symlinks instead: `ln -s <path>/acoremods/<module> <path>/azerothcore-wotlk/modules/<module>`.

#### Option B — Copy

Copy each `mod-*` folder directly into `azerothcore-wotlk/modules/`.

### Step 2 — Apply the required core patch

`mod-custom-items`, `mod-mount-progression`, and `mod-terror-zones` add `ScriptMgr` hooks
that stock AzerothCore does not have. **Without this patch they will not compile.** From
your AzerothCore checkout root:

```bash
git apply <path-to>/acoremods/core-patches/acore-core-hooks.patch
# on a newer/drifted core, retry with: git apply --3way <...>/acore-core-hooks.patch
```

See [core-patches/](core-patches/) for the full per-module breakdown of what the patch
changes. `mod-living-world` and `mod-dynamic-ah` need no core changes.

### Step 3 — Build

Re-run CMake and rebuild AzerothCore.
