#include "CustomItems.h"

#include "Config.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QueryResult.h"

#include <vector>

namespace mod_custom_items
{

namespace
{
    // Runtime donor map. Populated by LoadCustomItems; consulted by
    // the AllItemScript egress hook through GetDisplayDonor. Not
    // thread-protected — LoadCustomItems runs once at boot on the
    // world thread, and the map is immutable thereafter.
    std::unordered_map<uint32 /*custom*/, uint32 /*donor*/> _displayDonors;

    // Reverse index for the tooltip-query substitution path. Built
    // alongside `_displayDonors`. Multiple custom entries can share
    // a donor — e.g. several Pass-1 items might all alias to the
    // same vanilla axe — so the value is a vector. World-thread-
    // immutable post-boot.
    std::unordered_map<uint32 /*donor*/,
                        std::vector<uint32 /*custom*/>> _reverseDonors;
}

uint32 EvaluateRewriteEntry(
    uint32 inputEntry,
    std::unordered_map<uint32, uint32> const& donors)
{
    if (!IsCustomItemEntry(inputEntry))
        return inputEntry;
    auto it = donors.find(inputEntry);
    if (it == donors.end())
        return inputEntry;   // fail open — let the client see the raw entry
    return it->second;
}

uint32 GetDisplayDonor(uint32 customEntry)
{
    auto it = _displayDonors.find(customEntry);
    return it == _displayDonors.end() ? 0u : it->second;
}

uint32 RewriteWireEntry(uint32 inputEntry)
{
    return EvaluateRewriteEntry(inputEntry, _displayDonors);
}

uint32 PickCustomEntryForQuery(Player const* querier, uint32 wireEntry)
{
    if (!querier)
        return 0;
    auto it = _reverseDonors.find(wireEntry);
    if (it == _reverseDonors.end())
        return 0;
    // CMSG_ITEM_QUERY_SINGLE carries only the entry ID with no
    // per-instance GUID, so when the player owns BOTH a custom item
    // (rewritten on-wire to this donor) AND the real donor item, the
    // server can't tell which one the client is hovering over. The
    // custom case is the one we care about in production — donors
    // are pre-screened to have zero organic acquisition (no creature
    // drops, no vendors, no quest rewards), so a player legitimately
    // holding both never happens. For GM-led testing where
    // `.additem <donor>` injects the real item, accept the bleed:
    // the real donor item's tooltip will render as the custom item's
    // data while both share inventory.
    for (uint32 customEntry : it->second)
    {
        if (querier->GetItemByEntry(customEntry))
            return customEntry;
    }
    return 0;
}

void LoadCustomItems()
{
    _displayDonors.clear();
    _reverseDonors.clear();

    if (!sConfigMgr->GetOption<bool>("CustomItems.Enable", true))
    {
        LOG_INFO("module",
            "mod-custom-items: disabled by config; skipping load.");
        return;
    }

    // Step 1 — donors map. Loaded first so the custom-item loader
    // can validate that every custom row has a donor mapping.
    if (QueryResult dr = WorldDatabase.Query(
            "SELECT custom_entry, donor_entry "
            "FROM custom_item_display_donors"))
    {
        do
        {
            Field* f = dr->Fetch();
            uint32 ce = f[0].Get<uint32>();
            uint32 de = f[1].Get<uint32>();
            if (!IsCustomItemEntry(ce))
            {
                LOG_WARN("module",
                    "mod-custom-items: skipping donor row "
                    "custom_entry={} (outside reserved range "
                    "[{}, {})).", ce, kCustomItemEntryFloor,
                    kCustomItemEntryCeil);
                continue;
            }
            _displayDonors[ce] = de;
            _reverseDonors[de].push_back(ce);
        } while (dr->NextRow());
    }

    // Step 2 — custom items. Each row clones the donor's stock
    // ItemTemplate as its base, then applies the row's overrides
    // and policy flags. Per-row policy columns control which donor
    // fields get stripped vs. inherited (sockets, equip gating,
    // weapon procs, random affixes, vendor fields, bonding).
    QueryResult ir = WorldDatabase.Query(
        "SELECT entry, class, subclass, name, displayid, Quality, "
        "InventoryType, ItemLevel, RequiredLevel, description, "
        "stat_type1, stat_value1, stat_type2, stat_value2, "
        "stat_type3, stat_value3, stat_type4, stat_value4, "
        "stat_type5, stat_value5, stat_type6, stat_value6, "
        "stat_type7, stat_value7, stat_type8, stat_value8, "
        "stat_type9, stat_value9, stat_type10, stat_value10, "
        "dmg_min1, dmg_max1, dmg_type1, "
        "dmg_min2, dmg_max2, dmg_type2, "
        "delay, armor, "
        "strip_sockets, strip_equip_gating, strip_weapon_procs, "
        "strip_random_affixes, strip_vendor_fields, force_bonding "
        "FROM custom_item_template");
    if (!ir)
    {
        LOG_INFO("module",
            "mod-custom-items: no rows in custom_item_template.");
        return;
    }

    ItemTemplateContainer* store =
        sObjectMgr->GetMutableItemTemplateStore();
    if (!store)
    {
        LOG_ERROR("module",
            "mod-custom-items: GetMutableItemTemplateStore returned "
            "null; cannot inject custom items.");
        return;
    }

    uint32 injected = 0, skipped = 0;
    do
    {
        Field* f = ir->Fetch();
        uint32 entry         = f[0].Get<uint32>();
        uint8  classId       = f[1].Get<uint8>();
        uint8  subClassId    = f[2].Get<uint8>();
        std::string name     = f[3].Get<std::string>();
        uint32 displayId     = f[4].Get<uint32>();
        uint8  quality       = f[5].Get<uint8>();
        uint8  invType       = f[6].Get<uint8>();
        uint16 ilvl          = f[7].Get<uint16>();
        uint8  reqLevel      = f[8].Get<uint8>();
        std::string desc     = f[9].Get<std::string>();
        // 10 (stat_type1..10, stat_value1..10) = fields[10..29]
        // 30 (dmg_min1, dmg_max1, dmg_type1)   = fields[30..32]
        // 33 (dmg_min2, dmg_max2, dmg_type2)   = fields[33..35]
        // 36 (delay)                           = fields[36]
        // 37 (armor)                           = fields[37]
        bool stripSockets       = f[38].Get<uint8>() != 0;
        bool stripEquipGating   = f[39].Get<uint8>() != 0;
        bool stripWeaponProcs   = f[40].Get<uint8>() != 0;
        bool stripRandomAffixes = f[41].Get<uint8>() != 0;
        bool stripVendorFields  = f[42].Get<uint8>() != 0;
        bool forceBondingSet    = !f[43].IsNull();
        uint8 forceBonding      = forceBondingSet ? f[43].Get<uint8>() : 0;

        if (!IsCustomItemEntry(entry))
        {
            LOG_WARN("module",
                "mod-custom-items: skipping item row entry={} "
                "(outside reserved range [{}, {})).", entry,
                kCustomItemEntryFloor, kCustomItemEntryCeil);
            ++skipped;
            continue;
        }

        auto donorIt = _displayDonors.find(entry);
        if (donorIt == _displayDonors.end())
        {
            LOG_ERROR("module",
                "mod-custom-items: custom item entry={} has no "
                "donor mapping; skipping injection. Add a row to "
                "custom_item_display_donors.", entry);
            ++skipped;
            continue;
        }

        ItemTemplate const* donor =
            sObjectMgr->GetItemTemplate(donorIt->second);
        if (!donor)
        {
            LOG_ERROR("module",
                "mod-custom-items: donor entry={} for custom "
                "entry={} not found in _itemTemplateStore; "
                "skipping injection.", donorIt->second, entry);
            ++skipped;
            continue;
        }

        ItemTemplate clone = *donor;   // deep copy of all fields
        clone.ItemId        = entry;
        clone.Class         = classId;
        clone.SubClass      = subClassId;
        clone.Name1         = std::move(name);
        clone.DisplayInfoID = displayId;
        clone.Quality       = quality;
        clone.InventoryType = invType;
        clone.ItemLevel     = ilvl;
        clone.RequiredLevel = reqLevel;
        clone.Description   = std::move(desc);

        // Per-stat slots 1..10. Compact non-zero entries into the
        // leading positions and drive the StatsCount field accordingly
        // — that's how AC's native loader writes them.
        uint32 statsCount = 0;
        for (uint32 slot = 0; slot < 10; ++slot)
        {
            uint8 type  = f[10 + slot * 2].Get<uint8>();
            int32 value = f[11 + slot * 2].Get<int32>();
            if (!type || !value)
                continue;
            clone.ItemStat[statsCount].ItemStatType  = type;
            clone.ItemStat[statsCount].ItemStatValue = value;
            ++statsCount;
        }
        for (uint32 slot = statsCount; slot < 10; ++slot)
        {
            clone.ItemStat[slot].ItemStatType  = 0;
            clone.ItemStat[slot].ItemStatValue = 0;
        }
        clone.StatsCount = statsCount;

        // Damage slots 1..2 + delay + armor. Treat zero damage as
        // "inherit donor" so a row that doesn't customize damage
        // keeps whatever the donor had.
        if (float dmin = f[30].Get<float>(); dmin > 0.0f)
        {
            clone.Damage[0].DamageMin  = dmin;
            clone.Damage[0].DamageMax  = f[31].Get<float>();
            clone.Damage[0].DamageType = f[32].Get<uint8>();
        }
        if (float dmin2 = f[33].Get<float>(); dmin2 > 0.0f)
        {
            clone.Damage[1].DamageMin  = dmin2;
            clone.Damage[1].DamageMax  = f[34].Get<float>();
            clone.Damage[1].DamageType = f[35].Get<uint8>();
        }
        if (uint16 d = f[36].Get<uint16>(); d > 0)
            clone.Delay = d;
        if (uint32 a = f[37].Get<uint32>(); a > 0)
            clone.Armor = a;

        // Policy: vendor + quest leak prevention. When set, the row
        // shouldn't be vendorable or quest-bound. Default off so a
        // generic-mod consumer that wants vendorable cosmetics keeps
        // the donor's pricing and StartQuest.
        if (stripVendorFields)
        {
            clone.BuyPrice   = 0;
            clone.SellPrice  = 0;
            clone.StartQuest = 0;
        }

        // Policy: strip random affixes ("of the Monkey" suffix rolls).
        if (stripRandomAffixes)
        {
            clone.RandomProperty = 0;
            clone.RandomSuffix   = 0;
        }

        // Policy: strip donor's equip-gating restrictions. Without
        // this, a "requires Argent Crusade Revered" donor blocks
        // equip on every custom item that aliases to it. Custom
        // template's own RequiredLevel + Class+Subclass+InventoryType
        // remain authoritative for slot/spec eligibility.
        if (stripEquipGating)
        {
            clone.AllowableClass            = -1;
            clone.AllowableRace             = -1;
            clone.RequiredSkill             = 0;
            clone.RequiredSkillRank         = 0;
            clone.RequiredSpell             = 0;
            clone.RequiredHonorRank         = 0;
            clone.RequiredCityRank          = 0;
            clone.RequiredReputationFaction = 0;
            clone.RequiredReputationRank    = 0;
        }

        // Policy: bonding override. NULL = inherit donor; non-NULL
        // overrides (1=BoP, 2=BoU, 3=BoE, 4=Quest).
        if (forceBondingSet)
            clone.Bonding = forceBonding;

        // Policy: strip donor sockets. Inheriting them gave random
        // per-cell power swings (a 3-socket donor lets the player
        // slot 3 epic gems for ~+60 stats; a 0-socket donor gives
        // nothing) — and a caster-themed donor's socket bonus on a
        // STR-DPS item reads as nonsense. Zero everything.
        if (stripSockets)
        {
            for (uint32 i = 0; i < MAX_ITEM_PROTO_SOCKETS; ++i)
            {
                clone.Socket[i].Color   = 0;
                clone.Socket[i].Content = 0;
            }
            clone.socketBonus = 0;
        }

        // Policy: strip donor weapon procs. Heroic ICC weapon
        // donor's lifedrain / on-hit damage / chance-on-hit procs
        // would bleed into our custom weapons and dwarf the stat
        // tuning. Armor donors keep their on-equip auras (those are
        // stable stat-shaped effects, not random procs).
        if (stripWeaponProcs && clone.Class == 2)
        {
            for (uint32 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            {
                clone.Spells[i].SpellId               = 0;
                clone.Spells[i].SpellTrigger          = 0;
                clone.Spells[i].SpellCharges          = 0;
                clone.Spells[i].SpellPPMRate          = 0.0f;
                clone.Spells[i].SpellCooldown         = -1;
                clone.Spells[i].SpellCategory         = 0;
                clone.Spells[i].SpellCategoryCooldown = -1;
            }
        }

        (*store)[entry] = std::move(clone);
        ++injected;
    } while (ir->NextRow());

    sObjectMgr->RebuildItemTemplateFastStore();

    LOG_INFO("module",
        "mod-custom-items: injected {} custom item(s) from "
        "custom_item_template ({} donor mapping(s); "
        "{} row(s) skipped).",
        injected, _displayDonors.size(), skipped);
}

} // namespace mod_custom_items
