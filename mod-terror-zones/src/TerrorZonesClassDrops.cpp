// Slice 9 Pass 1 — class-targeted event-boss drops. Holds the
// runtime data load + (later) the TryClassDrop roll path. Mgr-method
// definitions only; pure helpers (encode/decode, archetype mapping)
// live in TerrorZonesMath.cpp.

#include "TerrorZonesMgr.h"

#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "LootMgr.h"
#include "Player.h"
#include "QueryResult.h"
#include "Util.h"

#include <ctime>

namespace mod_terror_zones
{

namespace
{
    // §4 level-band bucketing. Bands are 10-19, 20-29, ..., 70-79, 80
    // (8 brackets). Anything < 10 buckets to band 0; anything ≥ 80 to
    // band 7. The same bucketing the offline Python generator uses
    // when picking which (band, tier, archetype, slot) cell a zone
    // belongs to.
    constexpr uint8 BandIndexForLevel(uint8 lvl)
    {
        if (lvl >= 80) return 7;
        if (lvl < 10) return 0;
        return static_cast<uint8>((lvl - 10) / 10);
    }
}

void TerrorZonesMgr::LoadClassDropIndex()
{
    _classDropEntries.clear();

    QueryResult r = WorldDatabase.Query(
        "SELECT band_index, tier, archetype, slot_index, item_entry "
        "FROM terror_zones_event_boss_class_drops");
    if (!r)
    {
        LOG_INFO("module",
            "mod-terror-zones: no rows in "
            "terror_zones_event_boss_class_drops "
            "(Pass 1 not migrated yet?).");
        return;
    }

    uint32 loaded = 0, skipped = 0;
    do
    {
        Field* f = r->Fetch();
        uint8  band      = f[0].Get<uint8>();
        uint8  tierRaw   = f[1].Get<uint8>();
        uint8  archRaw   = f[2].Get<uint8>();
        uint8  slotRaw   = f[3].Get<uint8>();
        uint32 itemEntry = f[4].Get<uint32>();

        // SQL column ranges (T1 now drops items too — every tier
        // sits above its band's frontier):
        //   band_index 0..7, tier 1..5, archetype 0..4, slot 0..11
        // Archetype enum is 1-based (ARCHETYPE_NONE = 0 sentinel),
        // so SQL archetype column needs +1 at the boundary.
        if (tierRaw < TIER_1 || tierRaw > TIER_5
            || archRaw >= 5
            || slotRaw >= ARMOR_SLOT_COUNT
            || band >= 8)
        {
            LOG_WARN("module",
                "mod-terror-zones: skipping class-drop row "
                "(band={}, tier={}, archetype={}, slot={}, entry={}) "
                "— axis value out of range.",
                band, tierRaw, archRaw, slotRaw, itemEntry);
            ++skipped;
            continue;
        }
        uint32 expected = EncodeClassDropEntry(
            band, static_cast<Tier>(tierRaw),
            static_cast<Archetype>(archRaw + 1u),
            static_cast<ArmorSlot>(slotRaw));
        if (expected != itemEntry)
        {
            LOG_ERROR("module",
                "mod-terror-zones: class-drop row entry mismatch "
                "(band={}, tier={}, archetype={}, slot={}): "
                "row says {} but encoder says {}. Refusing to load "
                "this row — fix the SQL or regenerate.",
                band, tierRaw, archRaw, slotRaw,
                itemEntry, expected);
            ++skipped;
            continue;
        }
        if (!_classDropEntries.insert(itemEntry).second)
        {
            LOG_WARN("module",
                "mod-terror-zones: duplicate class-drop entry {} "
                "skipped.", itemEntry);
            ++skipped;
            continue;
        }
        ++loaded;
    } while (r->NextRow());

    LOG_INFO("module",
        "mod-terror-zones: loaded {} class-drop entries "
        "({} skipped).", loaded, skipped);
}

bool TerrorZonesMgr::TryClassDrop(Player const* player, Loot& loot)
{
    if (!player || !_enabled)
        return false;

    if (_debug)
        LOG_INFO("module",
            "mod-terror-zones: TryClassDrop entered — "
            "player_guid={} zone={} class={}",
            player->GetGUID().GetCounter(),
            player->GetZoneId(),
            static_cast<uint32>(player->getClass()));

    // Per-bundle dedup. Independent of _eventBossLootRolledBundles so
    // a single kill can fire blue/purple/gold AND a class drop, and
    // re-entry of TryClassDrop on the same bundle is a no-op.
    uint64 bundleKey = reinterpret_cast<uint64>(&loot);
    uint64 now = static_cast<uint64>(::time(nullptr));
    if (now - _classDropRolledBundlesClearedAt > 30
        || _classDropRolledBundles.size() > 10000)
    {
        _classDropRolledBundles.clear();
        _classDropRolledBundlesClearedAt = now;
    }
    if (!_classDropRolledBundles.insert(bundleKey).second)
    {
        if (_debug)
            LOG_INFO("module",
                "mod-terror-zones: class-drop skipped — bundle dedup");
        return false;
    }

    // Tier resolution. Event bosses get their source-rotation-slot
    // tier captured at spawn (boss spawn anchors are decoupled from
    // rotation zones — see ApplyEventBossGoldUplift comment). Fall
    // back to the player's zone rotation tier if the boss isn't
    // tracked (regular mob in an empowered zone).
    uint32 playerZone = player->GetZoneId();
    Tier tier = TIER_NONE;
    uint64 sourceGuid = loot.sourceWorldObjectGUID.GetRawValue();
    if (sourceGuid != 0)
    {
        auto it = _eventBossTierMap.find(sourceGuid);
        if (it != _eventBossTierMap.end())
            tier = it->second;
    }
    if (tier == TIER_NONE)
    {
        for (ActiveSlot const& s : _rotation.slots)
        {
            if (s.zoneId == playerZone)
            {
                tier = s.tier;
                break;
            }
        }
    }
    if (tier == TIER_NONE)
    {
        if (_debug)
            LOG_INFO("module",
                "mod-terror-zones: class-drop skipped — zone {} "
                "not in any rotation slot",
                playerZone);
        return false;
    }

    float chance = _classDropChance[tier];
    if (chance <= 0.0f)
    {
        if (_debug)
            LOG_INFO("module",
                "mod-terror-zones: class-drop skipped — "
                "DropChance.T{} is 0", static_cast<uint32>(tier));
        return false;
    }
    if (!roll_chance_f(chance * 100.0f))
    {
        if (_debug)
            LOG_INFO("module",
                "mod-terror-zones: class-drop skipped — "
                "tier={} chance={:.2f} roll missed",
                static_cast<uint32>(tier), chance);
        return false;
    }

    // Class+spec → archetype. GetMostPointsTalentTree is non-const
    // but read-only (it iterates the talent map without mutation);
    // const_cast keeps the const-correctness elsewhere intact.
    uint8 classId = player->getClass();
    uint8 specIdx = const_cast<Player*>(player)
                        ->GetMostPointsTalentTree();
    Archetype arch = ArchetypeForClassSpec(classId, specIdx);
    if (arch == ARCHETYPE_NONE)
    {
        if (_debug)
            LOG_INFO("module",
                "mod-terror-zones: class-drop skipped — "
                "ArchetypeForClassSpec(class={}, spec={}) = NONE",
                static_cast<uint32>(classId),
                static_cast<uint32>(specIdx));
        return false;
    }

    // Band derivation: tied to the PLAYER's level, not the zone's
    // level bracket. Mobs scale up to player level via the Slice 2
    // scaling system (a level-15 zone in T5 rotation fights at
    // level-80 difficulty if a level-80 player is around), so
    // anchoring loot to the zone band would mean level-80 players
    // get level-15 loot from level-80-difficulty fights. Player
    // level keeps reward + challenge symmetric.
    uint8 band = BandIndexForLevel(player->GetLevel());
    (void)playerZone;  // no longer used for band, kept for log context

    uint32 slotIdx = urand(0, ARMOR_SLOT_COUNT - 1);
    ArmorSlot slot = static_cast<ArmorSlot>(slotIdx);

    uint32 entry = EncodeClassDropEntry(band, tier, arch, slot);
    if (entry == 0)
    {
        if (_debug)
            LOG_INFO("module",
                "mod-terror-zones: class-drop skipped — encoder "
                "returned 0 (band={}, tier={}, arch={}, slot={})",
                static_cast<uint32>(band),
                static_cast<uint32>(tier),
                static_cast<uint32>(arch),
                static_cast<uint32>(slot));
        return false;
    }
    if (!_classDropEntries.count(entry))
    {
        if (_debug)
            LOG_INFO("module",
                "mod-terror-zones: class-drop skipped — entry {} "
                "not populated (band={}, tier={}, arch={}, slot={}). "
                "Cell missing from terror_zones_event_boss_class_drops.",
                entry,
                static_cast<uint32>(band),
                static_cast<uint32>(tier),
                static_cast<uint32>(arch),
                static_cast<uint32>(slot));
        return false;
    }

    // BoP behavior comes from item_template.bonding=1 set by the
    // generator; LootStoreItem args mirror TryEventBossDrop's
    // additive injection.
    LootStoreItem item(entry, 0, 100.0f, false,
                        LOOT_MODE_DEFAULT, 0, 1, 1);
    loot.AddItem(item);

    if (_debug)
        LOG_INFO("module",
            "mod-terror-zones: class-drop injected — entry={} "
            "tier={} archetype={} slot={} band={} "
            "player_guid={} class={} spec={} "
            "[bundle readback: items={} gold={} unlooted={}]",
            entry, static_cast<uint32>(tier),
            static_cast<uint32>(arch), static_cast<uint32>(slot),
            static_cast<uint32>(band),
            player->GetGUID().GetCounter(),
            static_cast<uint32>(classId),
            static_cast<uint32>(specIdx),
            static_cast<uint32>(loot.items.size()),
            loot.gold,
            static_cast<uint32>(loot.unlootedCount));
    return true;
}

} // namespace mod_terror_zones
