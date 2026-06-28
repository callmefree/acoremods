// Slice 3 + 5 — reward Mgr methods. `ApplyXpMultiplier`,
// `ApplyGoldMultiplier`, `ApplyQuestGoldMultiplier`, `TryTierBump`,
// `TryGatheringYieldBump`, `TryUniqueDrop`, plus the helpers they share
// (`TryGetSlotForZone`, `RollAxis`, `GetActiveFlavor`) and the two
// indices built at startup (`BuildRarityIndex`, `BuildUniqueDropIndex`).
//
// All Slice 5 reward paths read `ComputeAxisRoll` when tiers are
// enabled AND the slot's tier is non-zero; otherwise they fall back to
// the Slice 4 flat per-flavor overlay path for rollback insurance.

#include "TerrorZonesMgr.h"

#include "ItemTemplate.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <unordered_set>

namespace mod_terror_zones
{

namespace
{
    // (quality, band) → index key. Packs both into one uint32 so the map
    // lookup stays trivial.
    uint32 RarityKey(uint32 quality, uint32 band)
    {
        return (quality << 8) | (band & 0xFF);
    }
}

Flavor TerrorZonesMgr::GetActiveFlavor(uint32 zoneId) const
{
    if (!_flavorsEnabled || zoneId == 0)
        return FLAVOR_NONE;
    auto rot = std::atomic_load_explicit(&_rotationSnap,
                                          std::memory_order_acquire);
    if (!rot)
        return FLAVOR_NONE;
    for (ActiveSlot const& s : rot->slots)
        if (s.zoneId == zoneId)
            return s.flavor;
    return FLAVOR_NONE;
}

bool TerrorZonesMgr::TryGetSlotForZone(uint32 zoneId, ActiveSlot& out) const
{
    if (zoneId == 0)
        return false;
    auto rot = std::atomic_load_explicit(&_rotationSnap,
                                          std::memory_order_acquire);
    if (!rot)
        return false;
    for (ActiveSlot const& s : rot->slots)
        if (s.zoneId == zoneId)
        {
            out = s;
            return true;
        }
    return false;
}

float TerrorZonesMgr::RollAxis(ActiveSlot const& slot, RewardAxis axis) const
{
    auto rot = std::atomic_load_explicit(&_rotationSnap,
                                          std::memory_order_acquire);
    uint64 tickAt = rot ? rot->tickAt : 0;
    return ComputeAxisRoll(tickAt, slot.slotIndex, slot.flavor,
                            slot.tier, axis, _tierCfg);
}

void TerrorZonesMgr::ApplyXpMultiplier(uint32& amount, Player* player) const
{
    if (!IsRewardsEnabled() || amount == 0)
        return;
    if (!player || !player->IsInWorld())
        return;

    ActiveSlot slot;
    if (!TryGetSlotForZone(player->GetZoneId(), slot))
        return;

    uint32 before = amount;
    float effectiveMult;
    float rolledOrFlavor;
    char const* source;
    if (_tierEnabled && slot.tier != TIER_NONE)
    {
        rolledOrFlavor = RollAxis(slot, AXIS_XP);
        effectiveMult = _xpMultiplier * rolledOrFlavor;
        source = "tier";
    }
    else
    {
        rolledOrFlavor = (slot.flavor != FLAVOR_NONE)
                       ? _flavorXpBoost[slot.flavor - 1]
                       : 1.0f;
        effectiveMult = _xpMultiplier * rolledOrFlavor;
        source = "flavor";
    }
    amount = ComputeMultipliedValue(amount, effectiveMult);
    if (_debug)
        LOG_INFO("module",
                 "mod-terror-zones: xp multiplier zone={} flavor={} tier={} "
                 "from={} to={} (base x{:.2f} * {} x{:.3f} = x{:.3f})",
                 player->GetZoneId(), FlavorDisplayName(slot.flavor),
                 TierDisplayName(slot.tier),
                 before, amount, _xpMultiplier, source,
                 rolledOrFlavor, effectiveMult);
}

void TerrorZonesMgr::ApplyGoldMultiplier(uint32& gold, Player* player) const
{
    if (!IsRewardsEnabled() || gold == 0)
        return;
    if (!player || !player->IsInWorld())
        return;

    uint32 before = gold;
    uint32 zoneId = player->GetZoneId();
    ActiveSlot slot;
    if (!TryGetSlotForZone(zoneId, slot))
        return;

    // Loot gold is template-fixed (creature_template.mingold / maxgold),
    // so a level-20 mob scaled to 70 still drops level-20 amounts. Layer
    // a level-uplift factor on top: (scaledLevel / poolLevelMax)^exp, so
    // scaled encounters pay out at something like the level they feel.
    float levelFactor = 1.0f;
    uint8 scaledLevel = ComputeTargetLevel(zoneId);
    if (scaledLevel > 0 && _goldLevelRatioExp > 0.0f)
    {
        // `_pool` / `_poolIndex` are write-once at LoadPool.
        uint16 poolMax = 0;
        auto it = _poolIndex.find(zoneId);
        if (it != _poolIndex.end())
            poolMax = _pool[it->second].levelMax;
        if (poolMax > 0 && scaledLevel > poolMax)
        {
            float ratio = static_cast<float>(scaledLevel)
                        / static_cast<float>(poolMax);
            levelFactor = std::pow(ratio, _goldLevelRatioExp);
        }
    }

    float rolledOrFlavor;
    float effectiveMult;
    char const* source;
    if (_tierEnabled && slot.tier != TIER_NONE)
    {
        rolledOrFlavor = RollAxis(slot, AXIS_GOLD);
        effectiveMult = _goldMultiplier * levelFactor * rolledOrFlavor;
        source = "tier";
    }
    else
    {
        rolledOrFlavor = (slot.flavor != FLAVOR_NONE)
                       ? _flavorGoldBoost[slot.flavor - 1]
                       : 1.0f;
        effectiveMult = _goldMultiplier * levelFactor * rolledOrFlavor;
        source = "flavor";
    }
    gold = ComputeMultipliedValue(gold, effectiveMult);
    if (_debug)
        LOG_INFO("module",
                 "mod-terror-zones: loot gold multiplier zone={} flavor={} "
                 "tier={} mob_level={} from={} to={} "
                 "(flat x{:.2f} * level x{:.2f} * {} x{:.3f} = x{:.3f})",
                 zoneId, FlavorDisplayName(slot.flavor),
                 TierDisplayName(slot.tier),
                 static_cast<uint32>(scaledLevel),
                 before, gold, _goldMultiplier, levelFactor,
                 source, rolledOrFlavor, effectiveMult);
}

void TerrorZonesMgr::ApplyQuestGoldMultiplier(int32& moneyRew, Player* player) const
{
    if (!IsRewardsEnabled() || moneyRew <= 0)
        return;  // Negative quest costs and zero rewards stay untouched.
    if (!player || !player->IsInWorld())
        return;
    ActiveSlot slot;
    if (!TryGetSlotForZone(player->GetZoneId(), slot))
        return;

    int32 before = moneyRew;
    float rolledOrFlavor;
    float effectiveMult;
    char const* source;
    if (_tierEnabled && slot.tier != TIER_NONE)
    {
        rolledOrFlavor = RollAxis(slot, AXIS_GOLD);
        effectiveMult = _goldMultiplier * rolledOrFlavor;
        source = "tier";
    }
    else
    {
        rolledOrFlavor = (slot.flavor != FLAVOR_NONE)
                       ? _flavorGoldBoost[slot.flavor - 1]
                       : 1.0f;
        effectiveMult = _goldMultiplier * rolledOrFlavor;
        source = "flavor";
    }
    uint32 multiplied = ComputeMultipliedValue(
        static_cast<uint32>(moneyRew), effectiveMult);
    if (multiplied > static_cast<uint32>(std::numeric_limits<int32>::max()))
        multiplied = static_cast<uint32>(std::numeric_limits<int32>::max());
    moneyRew = static_cast<int32>(multiplied);
    if (_debug)
        LOG_INFO("module",
                 "mod-terror-zones: quest gold multiplier zone={} flavor={} "
                 "tier={} from={} to={} "
                 "(base x{:.2f} * {} x{:.3f} = x{:.3f})",
                 player->GetZoneId(), FlavorDisplayName(slot.flavor),
                 TierDisplayName(slot.tier),
                 before, moneyRew, _goldMultiplier,
                 source, rolledOrFlavor, effectiveMult);
}

void TerrorZonesMgr::BuildRarityIndex()
{
    if (_rarityIndexBuilt)
        return;

    _rarityIndex.clear();
    ItemTemplateContainer const* store = sObjectMgr->GetItemTemplateStore();
    if (!store)
    {
        LOG_WARN("module",
                 "mod-terror-zones: item template store unavailable, "
                 "tier bumps disabled.");
        _rarityIndexBuilt = true;
        return;
    }

    uint32 considered = 0;
    for (auto const& kv : *store)
    {
        ItemTemplate const& tmpl = kv.second;
        if (tmpl.Quality < ITEM_QUALITY_NORMAL || tmpl.Quality > ITEM_QUALITY_EPIC)
            continue;
        // Skip items with no RequiredLevel — they are usually keys, quest
        // items, and other things that break when substituted.
        if (tmpl.RequiredLevel == 0)
            continue;
        if (tmpl.RequiredLevel > 80)
            continue;
        uint32 band = tmpl.RequiredLevel / _levelBandWidth;
        _rarityIndex[RarityKey(tmpl.Quality, band)].push_back(tmpl.ItemId);
        ++considered;
    }

    _rarityIndexBuilt = true;
    LOG_INFO("module",
             "mod-terror-zones: rarity index built ({} items across {} buckets).",
             considered, static_cast<uint32>(_rarityIndex.size()));
}

bool TerrorZonesMgr::TryTierBump(Player const* player, ::LootStoreItem* item)
{
    if (!IsRewardsEnabled() || !item)
        return false;
    if (!player || !player->IsInWorld())
        return false;

    uint32 zoneId = player->GetZoneId();
    ActiveSlot slot;
    if (!TryGetSlotForZone(zoneId, slot))
        return false;

    ItemTemplate const* tmpl = sObjectMgr->GetItemTemplate(item->itemid);
    if (!tmpl)
        return false;
    if (tmpl->Quality >= _maxBumpQuality)
        return false;
    // Skip keys / quest items / other no-required-level items — substituting
    // them for level-band gear would break unrelated game systems.
    if (tmpl->RequiredLevel == 0)
        return false;

    // Single RNG stream for Slice 3 tier bumps. Seeded from steady_clock
    // on first use; single-thread open-world loot keeps this safe.
    static thread_local std::mt19937_64 engine(
        static_cast<uint64>(
            std::chrono::steady_clock::now().time_since_epoch().count()));

    // Slice 5 — tier roll replaces the Slice-4 additive overlay. The
    // Slice 3 master gate (`_tierBumpChance`) stays as a kill-switch:
    // setting it to 0 disables bumps entirely regardless of tier config.
    float effectiveChance;
    char const* source;
    if (_tierEnabled && slot.tier != TIER_NONE)
    {
        if (_tierBumpChance <= 0.0f)
            return false;
        effectiveChance = RollAxis(slot, AXIS_TIER_BUMP);
        source = "tier";
    }
    else
    {
        if (_tierBumpChance <= 0.0f)
            return false;
        float flavorAdd = (slot.flavor != FLAVOR_NONE)
                        ? _flavorTierBumpAdd[slot.flavor - 1]
                        : 0.0f;
        effectiveChance = _tierBumpChance + flavorAdd;
        source = "flavor";
    }
    if (effectiveChance < 0.0f) effectiveChance = 0.0f;
    if (effectiveChance > 1.0f) effectiveChance = 1.0f;

    std::uniform_real_distribution<float> chance(0.0f, 1.0f);
    if (chance(engine) >= effectiveChance)
        return false;

    // Level-band driven by the SCALED mob level, not the rolled item's
    // baseline RequiredLevel. A level-70 mob in an empowered zone should
    // drop a ~70 blue instead of the level-19 blue that matched the
    // untouched template. Falls back to the item's own RequiredLevel on
    // the defensive path where ComputeTargetLevel returns 0.
    uint8 scaledLevel = ComputeTargetLevel(zoneId);
    uint32 referenceLevel = (scaledLevel > 0)
                          ? static_cast<uint32>(scaledLevel)
                          : tmpl->RequiredLevel;
    if (referenceLevel > 80)
        referenceLevel = 80;

    uint32 targetQuality = tmpl->Quality + 1;
    uint32 band = referenceLevel / _levelBandWidth;
    int32 tol = static_cast<int32>(_tierBumpLevelTolerance
                                   / _levelBandWidth);
    if (tol < 1) tol = 1;

    // `_rarityIndex` is write-once at BuildRarityIndex.
    std::vector<uint32> candidates;
    for (int32 d = -tol; d <= tol; ++d)
    {
        int32 b = static_cast<int32>(band) + d;
        if (b < 0) continue;
        auto it = _rarityIndex.find(RarityKey(targetQuality,
                                              static_cast<uint32>(b)));
        if (it != _rarityIndex.end())
            candidates.insert(candidates.end(),
                              it->second.begin(), it->second.end());
    }

    if (candidates.empty())
        return false;

    std::uniform_int_distribution<size_t> pick(0, candidates.size() - 1);
    uint32 newId = candidates[pick(engine)];
    if (newId == item->itemid)
        return false;  // Trivial dedup.

    if (_debug)
        LOG_INFO("module",
                 "mod-terror-zones: tier bump zone={} flavor={} tier={} "
                 "mob_level={} from_item={} (q={}, reqLvl={}) "
                 "to_item={} (q={}) effective_chance={:.3f} source={}",
                 zoneId, FlavorDisplayName(slot.flavor),
                 TierDisplayName(slot.tier),
                 static_cast<uint32>(scaledLevel),
                 item->itemid, static_cast<uint32>(tmpl->Quality),
                 tmpl->RequiredLevel, newId, targetQuality,
                 effectiveChance, source);

    item->itemid = newId;
    return true;
}

// --- Prospector's gathering-yield overlay (plan §9.1) ---

bool TerrorZonesMgr::TryGatheringYieldBump(Player const* player,
                                            LootStoreItem* item,
                                            char const* storeName)
{
    if (!IsFlavorsEnabled() || !item)
        return false;
    if (!player || !player->IsInWorld())
        return false;
    if (!IsGatheringStore(storeName))
        return false;
    uint32 zoneId = player->GetZoneId();
    ActiveSlot slot;
    if (!TryGetSlotForZone(zoneId, slot))
        return false;

    static thread_local std::mt19937_64 engine(
        static_cast<uint64>(
            std::chrono::steady_clock::now().time_since_epoch().count())
        ^ 0xA5A5A5A5A5A5A5A5ULL);  // offset seed so tier-bump and
                                   // gathering don't share a stream.

    // Slice 5 — gathering now fires for every flavor (flavor-as-bias,
    // not flavor-as-gate). The rolled yield mult reads from the tier
    // roll; bonus-chance (how often the yield roll even fires) remains
    // a flat config knob since it's "how often a gathering encounter
    // carries an empowered upside" not a tier-intensity axis.
    std::uniform_real_distribution<float> chance(0.0f, 1.0f);
    if (chance(engine) >= _flavorGatheringBonusChance)
        return false;
    if (_flavorGatheringBonusChance <= 0.0f)
        return false;

    float yieldMult;
    char const* source;
    if (_tierEnabled && slot.tier != TIER_NONE)
    {
        yieldMult = RollAxis(slot, AXIS_GATHERING);
        source = "tier";
    }
    else
    {
        // Slice-4 fallback — only Prospector's bumps yield.
        if (slot.flavor != FLAVOR_PROSPECTORS)
            return false;
        yieldMult = _flavorGatheringYieldMult;
        source = "flavor";
    }
    if (yieldMult <= 1.0f)
        return false;

    uint32 before = item->maxcount;
    uint32 bumped = static_cast<uint32>(
        std::ceil(static_cast<float>(item->maxcount) * yieldMult));
    if (bumped > std::numeric_limits<uint8>::max())
        bumped = std::numeric_limits<uint8>::max();
    if (bumped <= before)
        return false;
    item->maxcount = static_cast<uint8>(bumped);

    if (_debug)
        LOG_INFO("module",
                 "mod-terror-zones: gathering yield bump zone={} store={} "
                 "flavor={} tier={} item={} maxcount from={} to={} "
                 "(x{:.3f} source={})",
                 zoneId, storeName,
                 FlavorDisplayName(slot.flavor),
                 TierDisplayName(slot.tier),
                 item->itemid,
                 before, static_cast<uint32>(item->maxcount),
                 yieldMult, source);
    return true;
}

// --- Unique empowered-only drops (plan §10) ---

void TerrorZonesMgr::BuildUniqueDropIndex()
{
    if (_uniqueDropsBuilt)
        return;
    for (uint32 i = 0; i <= FLAVOR_MAX; ++i)
        _uniqueDropsByFlavor[i].clear();

    QueryResult r = WorldDatabase.Query(
        "SELECT item_id, flavor, level_min, level_max, weight, enabled "
        "FROM terror_zones_unique_drops");
    if (!r)
    {
        LOG_WARN("module",
                 "mod-terror-zones: terror_zones_unique_drops missing or empty; "
                 "unique drops disabled at runtime.");
        _uniqueDropsBuilt = true;
        return;
    }

    uint32 loaded = 0, skipped = 0;
    do
    {
        Field* f = r->Fetch();
        uint32 itemId = f[0].Get<uint32>();
        uint8  flavor = f[1].Get<uint8>();
        uint16 lMin   = f[2].Get<uint16>();
        uint16 lMax   = f[3].Get<uint16>();
        uint32 weight = f[4].Get<uint32>();
        bool   enabled = f[5].Get<uint8>() != 0;
        if (!enabled || flavor > FLAVOR_MAX)
        {
            ++skipped;
            continue;
        }
        if (!sObjectMgr->GetItemTemplate(itemId))
        {
            LOG_WARN("module",
                     "mod-terror-zones: unique drop item_id={} does not exist "
                     "in item_template, skipping.", itemId);
            ++skipped;
            continue;
        }
        _uniqueDropsByFlavor[flavor].push_back(
            UniqueDropEntry{itemId, weight, lMin, lMax});
        ++loaded;
    } while (r->NextRow());

    _uniqueDropsBuilt = true;
    LOG_INFO("module",
             "mod-terror-zones: unique drops index built ({} items loaded, "
             "{} skipped).", loaded, skipped);
}

void TerrorZonesMgr::TryUniqueDrop(Player const* player, Loot* loot,
                                    uint32 zoneId)
{
    if (!_flavorsEnabled || !_flavorUniquesEnabled || !loot || !player)
        return;
    ActiveSlot slot;
    if (!TryGetSlotForZone(zoneId, slot))
        return;
    if (slot.flavor == FLAVOR_NONE)
        return;

    // Slice 5 — unique-drop chance reads from the tier roll instead of
    // the flat Slice-4 BaseChance. Base stays as a kill-switch (0 = off).
    float uniqueChance;
    char const* source;
    if (_tierEnabled && slot.tier != TIER_NONE)
    {
        if (_flavorUniquesBaseChance <= 0.0f)
            return;
        uniqueChance = RollAxis(slot, AXIS_UNIQUES);
        source = "tier";
    }
    else
    {
        if (_flavorUniquesBaseChance <= 0.0f)
            return;
        uniqueChance = _flavorUniquesBaseChance;
        source = "flavor";
    }
    if (uniqueChance <= 0.0f)
        return;

    uint8 scaledLevel = ComputeTargetLevel(zoneId);
    if (_flavorUniquesMinMobLevel > 0
        && static_cast<uint32>(scaledLevel) < _flavorUniquesMinMobLevel)
        return;

    // Per-bundle dedup — OnBeforeDropAddItem fires once per rolled item,
    // but uniques should roll AT MOST once per loot bundle. Tracked via
    // Loot* pointer identity (stable within a single bundle's lifetime).
    uint64 lootKey = static_cast<uint64>(
        reinterpret_cast<uintptr_t>(loot));
    {
        uint64 now = static_cast<uint64>(::time(nullptr));
        // Periodic clear — cheap since we only hit this path in empowered
        // zones. 30s window is plenty — no bundle lives that long.
        if (now - _uniqueRolledBundlesClearedAt > 30
            || _uniqueRolledBundles.size() > 10000)
        {
            _uniqueRolledBundles.clear();
            _uniqueRolledBundlesClearedAt = now;
        }
        if (!_uniqueRolledBundles.insert(lootKey).second)
            return;  // Already rolled for this bundle.
    }

    static thread_local std::mt19937_64 engine(
        static_cast<uint64>(
            std::chrono::steady_clock::now().time_since_epoch().count())
        ^ 0x3C3C3C3C3C3C3C3CULL);

    std::uniform_real_distribution<float> chance(0.0f, 1.0f);
    if (chance(engine) >= uniqueChance)
        return;

    // Candidate pool: flavor-tagged entries + wildcard (flavor=0) entries
    // whose level band overlaps the mob's scaled level.
    std::vector<UniqueDropEntry> candidates;
    uint16 mobLevel = static_cast<uint16>(scaledLevel);
    if (mobLevel == 0)
        mobLevel = 1;
    // `_uniqueDropsByFlavor` is write-once at BuildUniqueDropIndex.
    for (uint32 src : {static_cast<uint32>(slot.flavor),
                        static_cast<uint32>(FLAVOR_NONE)})
    {
        for (UniqueDropEntry const& e : _uniqueDropsByFlavor[src])
        {
            if (mobLevel < e.levelMin || mobLevel > e.levelMax)
                continue;
            if (e.weight == 0)
                continue;
            candidates.push_back(e);
        }
    }
    if (candidates.empty())
        return;

    uint64 total = 0;
    for (UniqueDropEntry const& e : candidates)
        total += e.weight;
    if (total == 0)
        return;

    uint64 roll = static_cast<uint64>(
        std::uniform_int_distribution<uint64>(0, total - 1)(engine));
    uint64 acc = 0;
    uint32 pickedItem = 0;
    for (UniqueDropEntry const& e : candidates)
    {
        acc += e.weight;
        if (roll < acc)
        {
            pickedItem = e.itemId;
            break;
        }
    }
    if (pickedItem == 0)
        return;

    // Additive — inject into the loot's item list without displacing
    // anything. LootStoreItem fields: (itemid, reference=0, chance=100,
    // needs_quest=false, lootmode=LOOT_MODE_DEFAULT, groupid=0,
    // mincount=1, maxcount=1). lootmode must be non-zero or
    // LootMgr's IsValid path would flag the item as invalid.
    LootStoreItem bonus(pickedItem, 0, 100.0f, false,
                        LOOT_MODE_DEFAULT, 0, 1, 1);
    loot->AddItem(bonus);

    if (_debug)
        LOG_INFO("module",
                 "mod-terror-zones: unique drop zone={} flavor={} tier={} "
                 "mob_level={} item={} chance={:.3f} source={} "
                 "(pool size={})",
                 zoneId, FlavorDisplayName(slot.flavor),
                 TierDisplayName(slot.tier),
                 static_cast<uint32>(scaledLevel),
                 pickedItem, uniqueChance, source,
                 static_cast<uint32>(candidates.size()));
}

} // namespace mod_terror_zones
