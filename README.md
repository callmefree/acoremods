# acoremods

A collection of custom [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) modules authored by jfalcos.

## Modules

| Module | Description |
| ------ | ----------- |
| `mod-custom-items` | Custom item definitions and handlers. |
| `mod-living-world` | Dynamic world / living-world behaviors. |
| `mod-mount-progression` | Mount progression system. |
| `mod-terror-zones` | Rotating high-difficulty "terror" zones. |

## Installation

AzerothCore discovers modules as direct children of its `modules/` directory.
Each module in this repo must therefore appear at `azerothcore-wotlk/modules/<module-name>`.

Pick one of the following:

### Option A — Directory junctions (keep the monorepo separate, recommended on Windows)

Clone this repo anywhere, then junction each module into your AzerothCore `modules/` folder:

```powershell
$src = "<path-to>\azerothcore-wotlk\modules"
$dst = "<path-to>\acoremods"
foreach ($m in @("mod-custom-items","mod-living-world","mod-mount-progression","mod-terror-zones")) {
  New-Item -ItemType Junction -Path (Join-Path $src $m) -Target (Join-Path $dst $m)
}
```

On Linux/macOS use symlinks instead: `ln -s <path>/acoremods/<module> <path>/azerothcore-wotlk/modules/<module>`.

### Option B — Copy

Copy each `mod-*` folder directly into `azerothcore-wotlk/modules/`.

After installing, re-run CMake and rebuild AzerothCore.
