# Core patches

Some modules in this repo require new `ScriptMgr` hooks (and a few call sites) that
**do not exist in stock AzerothCore**. Those edits live in the core tree, not in the
module folders, so they are captured here as a patch you re-apply after cloning or
updating AzerothCore.

> Without this patch the affected modules **will not compile** — you'll get missing
> `sScriptMgr->On...` symbol errors.

## Applying

From your AzerothCore checkout root:

```bash
git apply /path/to/acoremods/core-patches/acore-core-hooks.patch
# if context has drifted on a newer AzerothCore, try a 3-way merge:
git apply --3way /path/to/acoremods/core-patches/acore-core-hooks.patch
```

Then re-run CMake and rebuild.

To confirm it's already applied: `git apply --check -R acore-core-hooks.patch` from the
core root succeeds when the patch is present.

## Why one combined patch (not per-module)

`mod-custom-items` and `mod-mount-progression` both add hooks to the **same** dispatcher
files (`ScriptMgr.h`, `WorldScript.h`, `WorldScript.cpp`) on adjacent lines. Independent
per-module patches would conflict with each other on those shared hunks, so the changes
ship as a single patch. The per-module breakdown below documents what each module needs.

## What each module needs

### mod-custom-items
Inject custom item templates and rewrite on-wire entries so the client renders a known
"donor" appearance for unknown custom items.

| File | Change |
|------|--------|
| `Scripting/ScriptDefines/WorldScript.{h,cpp}` | `OnAfterLoadItemTemplates` hook + `WORLDHOOK_ON_AFTER_LOAD_ITEM_TEMPLATES` |
| `World/World.cpp` | Call `OnAfterLoadItemTemplates()` after `LoadItemTemplates()` |
| `Scripting/ScriptDefines/AllItemScript.{h,cpp}` | `OnItemBuildValuesUpdate`, `OnItemQueryTemplate` + `RewriteItemFieldOnEgress` helper |
| `Entities/Object/Object.cpp` | One-line egress: route update fields through `RewriteItemFieldOnEgress` |
| `Handlers/ItemHandler.cpp` | Call `OnItemQueryTemplate`; keep wire `ItemId` == queried entry |
| `Globals/ObjectMgr.{h,cpp}` | `GetMutableItemTemplateStore()` + `RebuildItemTemplateFastStore()` |
| `Scripting/ScriptMgr.h` | Declarations for the above |

### mod-mount-progression
Inject custom mount spells before `SpellMgr` builds, and rewrite the displayed aura spell
ID on outgoing packets.

| File | Change |
|------|--------|
| `Scripting/ScriptDefines/WorldScript.{h,cpp}` | `OnAfterLoadDBCStores` hook + `WORLDHOOK_ON_AFTER_LOAD_DBC_STORES` |
| `World/World.cpp` | Call `OnAfterLoadDBCStores()` after `LoadDBCStores()` |
| `Scripting/ScriptDefines/UnitScript.{h,cpp}` | `OnAuraBuildUpdatePacket` hook + `UNITHOOK_ON_AURA_BUILD_UPDATE_PACKET` |
| `Spells/Auras/SpellAuras.cpp` | Route the aura `spellId` through the hook in `BuildUpdatePacket` |
| `Scripting/ScriptMgr.h` | Declarations for the above |

### mod-terror-zones
Adjust quest money rewards.

| File | Change |
|------|--------|
| `Scripting/ScriptDefines/PlayerScript.{h,cpp}` | `OnPlayerQuestComputeMoney` hook + `PLAYERHOOK_ON_QUEST_COMPUTE_MONEY` |
| `Entities/Player/PlayerQuest.cpp` | Call `OnPlayerQuestComputeMoney()` in `RewardQuest` before applying money |
| `Scripting/ScriptMgr.h` | Declaration |

> `mod-terror-zones` also depends on `mod-custom-items` (it seeds rows into the custom-items
> tables for class-based drops), so it indirectly needs the custom-items hooks too.

### mod-living-world, mod-dynamic-ah
No core changes required.
