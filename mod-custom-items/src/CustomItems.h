#ifndef MOD_CUSTOM_ITEMS_H
#define MOD_CUSTOM_ITEMS_H

#include "Define.h"
#include <unordered_map>

class Player;

// Custom items via the display-donor pattern. Module-owned
// `custom_item_template` rows are injected into AC's
// `_itemTemplateStore` at boot through the core hook
// `WorldScript::OnAfterLoadItemTemplates`. The wire-side rendering
// trick — outgoing OBJECT_FIELD_ENTRY rewritten to a "donor" entry
// the client already knows — rides `AllItemScript::OnItemBuildValues
// Update`, consulting the `custom_item_display_donors` map.
//
// Extracted from mod-terror-zones (Slice 9) so other consumer mods
// can ship custom items without a client patch via the same shared
// tables and core hooks.

namespace mod_custom_items
{

// Reserved entry-ID window for module-owned custom items. Stock AC
// `item_template` max entry is 56806 (verified 2026-04-25); the
// 700000 floor sits 12.4× clear and safely above the AC custom
// convention of >= 200000. Widened from the original [700000, 710000)
// to give consumer mods room to claim sub-ranges by convention
// (terror-zones occupies 700000-709999, future mods take the next
// thousand-aligned slot, etc.).
constexpr uint32 kCustomItemEntryFloor = 700000;
constexpr uint32 kCustomItemEntryCeil  = 800000;   // exclusive

// True when the entry sits inside the module's reserved window.
[[nodiscard]] constexpr bool IsCustomItemEntry(uint32 entry)
{
    return entry >= kCustomItemEntryFloor && entry < kCustomItemEntryCeil;
}

// Pure helper — given a custom entry and a donors map, return the
// entry that should be packed onto the wire. Out-of-range entries
// pass through unchanged. Custom entries with no mapping ALSO pass
// through unchanged (the loader logs a warning at startup); this
// matches the "fail open" stance the mount-progression icon-donor
// path uses for spells without donors. Unit-tested.
uint32 EvaluateRewriteEntry(
    uint32 inputEntry,
    std::unordered_map<uint32, uint32> const& donors);

// Load both `custom_item_display_donors` (donor map) and
// `custom_item_template` (custom items) and inject the latter into
// AC's `_itemTemplateStore`. Each custom row clones the donor's
// stock ItemTemplate as its base, then applies the overrides
// (entry/name/quality/displayid/etc.) plus the per-row policy flags
// (strip_sockets / strip_equip_gating / strip_weapon_procs /
// strip_random_affixes / strip_vendor_fields / force_bonding).
// Called from the OnAfterLoadItemTemplates core hook.
void LoadCustomItems();

// Read-only access to the runtime donors map used by the egress
// rewrite hook. Returns 0 when there's no mapping for the entry.
uint32 GetDisplayDonor(uint32 customEntry);

// Hot-path egress helper used by the AllItemScript hook. Wraps
// `EvaluateRewriteEntry` with the module's runtime donors map.
// Returns the input entry unchanged for out-of-range entries.
uint32 RewriteWireEntry(uint32 inputEntry);

// Tooltip-query substitution helper. When the querier owns a custom
// item whose donor entry equals `wireEntry`, returns the custom item's
// real entry (in the [700000, 800000) range). Returns 0 when no
// substitution applies — caller leaves the original ItemTemplate
// untouched. Walks the querier's inventory through Player::GetItemByEntry
// for each candidate custom entry that maps to wireEntry.
uint32 PickCustomEntryForQuery(::Player const* querier, uint32 wireEntry);

} // namespace mod_custom_items

#endif
