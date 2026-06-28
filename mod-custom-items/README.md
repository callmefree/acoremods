# mod-custom-items

A framework for shipping **custom items without a client patch** on AzerothCore (WotLK 3.3.5a).

Custom item rows live in the reserved entry-ID window `[700000, 800000)`. Each custom
entry is mapped to a "donor" stock entry the unmodified client already knows from its
local `Item.dbc` / `ItemDisplayInfo.dbc`, so the client renders the donor's icon and 3D
model while the server serves the custom stats and tooltip.

## How it works

- **`custom_item_template`** — module-owned table (`LIKE item_template` + policy columns)
  holding the custom item rows.
- **`custom_item_display_donors`** — maps each custom entry to its donor stock entry.
- `WorldScript::OnAfterLoadItemTemplates` injects the custom rows into AzerothCore's
  `_itemTemplateStore` at boot.
- `AllItemScript::OnItemBuildValuesUpdate` rewrites `OBJECT_FIELD_ENTRY` on outgoing
  update packets to the donor entry (client renders the donor's appearance).
- `AllItemScript::OnItemQueryTemplate` substitutes the custom row back into tooltip
  query responses for players who own the item.

Consumer modules (e.g. [mod-terror-zones](../mod-terror-zones)) seed rows into the two
shared tables and claim sub-ranges by convention — no C++ wiring required for SQL-only
consumers.

## Configuration

See [`conf/mod_custom_items.conf.dist`](conf/mod_custom_items.conf.dist).

| Setting | Default | Description |
| ------- | ------- | ----------- |
| `CustomItems.Enable` | `1` | Master switch. When `0`, the loader is skipped, the wire-rewrite hook becomes a no-op, and tooltip substitution is disabled. Existing tables are left in place. |

## Core changes required

This module needs `ScriptMgr` hooks that stock AzerothCore lacks — it **will not compile**
without them. Apply [`core-patches/acore-core-hooks.patch`](../core-patches/) to your core
checkout. This module's slice adds: `WorldScript::OnAfterLoadItemTemplates`,
`AllItemScript::OnItemBuildValuesUpdate` / `OnItemQueryTemplate` (+ the
`RewriteItemFieldOnEgress` egress helper and the `Object::BuildValuesUpdate` call site),
the `ItemHandler` query substitution, and `ObjectMgr::GetMutableItemTemplateStore()` /
`RebuildItemTemplateFastStore()`. See [core-patches/](../core-patches/) for details.

## Installation

See the [repository README](../README.md) for how to junction/symlink this module into
your AzerothCore `modules/` directory and apply the core patch, then re-run CMake and rebuild.
