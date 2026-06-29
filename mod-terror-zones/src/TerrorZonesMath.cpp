// Pure, Mgr-independent math for mod-terror-zones. Everything here is
// a free function + POD data — no Player*, no Creature*, no DB access,
// no globals. Unit-tested in tests/TerrorZones{Selection,Scaling,Reward,
// Flavor,Gathering,Tier,Roll}Tests.cpp.
//
// Consolidated in a Slice 5 post-landing refactor to shrink Mgr.cpp and
// keep the pure layer cleanly separable from orchestration.

#include "TerrorZonesMgr.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <unordered_set>

namespace mod_terror_zones
{

// -----------------------------------------------------------------------------
// StdRng
// -----------------------------------------------------------------------------

StdRng::StdRng(uint64 seed) : _engine(seed) {}

uint32 StdRng::NextUInt(uint32 maxExclusive)
{
    if (maxExclusive == 0)
        return 0;
    std::uniform_int_distribution<uint32> dist(0, maxExclusive - 1);
    return dist(_engine);
}

// -----------------------------------------------------------------------------
// Slice 1 — zone selection (pure)
// -----------------------------------------------------------------------------

namespace
{
    double ZoneBaseWeight(PoolEntry const& z,
                          std::vector<uint8> const& targets,
                          SelectionConfig const& cfg)
    {
        if (targets.empty())
            return static_cast<double>(cfg.weightNear);  // flat

        double mid = (static_cast<double>(z.levelMin)
                    + static_cast<double>(z.levelMax)) / 2.0;

        uint8 minT = 0xFF, maxT = 0;
        bool inWindow = false;
        for (uint8 t : targets)
        {
            if (t < minT) minT = t;
            if (t > maxT) maxT = t;
            if (std::fabs(mid - static_cast<double>(t))
                <= static_cast<double>(cfg.levelWindow))
                inWindow = true;
        }

        if (inWindow)
            return static_cast<double>(cfg.weightNear);

        int lw = static_cast<int>(cfg.levelWindow);
        if (static_cast<int>(z.levelMax) + lw < static_cast<int>(minT))
            return static_cast<double>(cfg.weightBelow);
        if (static_cast<int>(z.levelMin) > static_cast<int>(maxT) + lw)
            return static_cast<double>(cfg.weightAbove);
        return static_cast<double>(cfg.weightOverlap);
    }
}

std::vector<uint32> SelectZones(
    std::vector<PoolEntry> const& pool,
    std::vector<uint8> const& targets,
    std::vector<uint32> const& recentZoneIds,
    SelectionConfig const& cfg,
    IRng& rng)
{
    std::vector<uint32> picks;
    if (pool.empty() || cfg.slotCount == 0)
        return picks;

    std::unordered_set<uint32> recent(recentZoneIds.begin(),
                                      recentZoneIds.end());

    struct Candidate { size_t idx; double weight; };
    std::vector<Candidate> remaining;
    remaining.reserve(pool.size());

    for (size_t i = 0; i < pool.size(); ++i)
    {
        PoolEntry const& z = pool[i];
        if (!z.enabled)
            continue;
        double w = ZoneBaseWeight(z, targets, cfg);
        if (recent.count(z.zoneId))
            w *= cfg.recencyMultiplier;
        if (w > 0.0)
            remaining.push_back({i, w});
    }

    while (picks.size() < cfg.slotCount && !remaining.empty())
    {
        double total = 0.0;
        for (auto const& c : remaining)
            total += c.weight;
        if (total <= 0.0)
            break;

        // Weighted pick via scaled uint32 roll for determinism with IRng.
        constexpr uint32 RESOLUTION = 1'000'000;
        uint32 roll = rng.NextUInt(RESOLUTION);
        double target = (static_cast<double>(roll) / RESOLUTION) * total;

        size_t chosen = 0;
        double acc = 0.0;
        for (size_t k = 0; k < remaining.size(); ++k)
        {
            acc += remaining[k].weight;
            if (target < acc)
            {
                chosen = k;
                break;
            }
            chosen = k;
        }

        picks.push_back(pool[remaining[chosen].idx].zoneId);
        remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(chosen));
    }

    return picks;
}

// -----------------------------------------------------------------------------
// Slice 2 — combat scaling (pure helpers)
// -----------------------------------------------------------------------------

uint8 ComputeTargetLevelPure(bool zoneIsEmpowered,
                              uint8 highestOnlineInZone,
                              uint8 zoneLevelMin,
                              uint8 zoneTier,
                              uint8 maxPlayerLevel)
{
    if (!zoneIsEmpowered)
        return 0;
    // Cap mob level at the player population (so a level-80 in
    // Westfall T5 fights level-80 mobs, a level-30 fights level-30
    // mobs), bounded by the zone's natural minimum on the low end
    // and `maxPlayerLevel + zoneTier` on the high end. Old logic
    // had a hard floor at 80 which broke "revisit old zones"
    // because Westfall mobs always read as level 80 regardless of
    // player level. The `+ zoneTier` ceiling lets max-level
    // players visit any TZ for a tougher version of that zone
    // (T5 with maxPlayerLevel=80 → level 85 cap).
    // maxPlayerLevel is a config knob (TerrorZones.MaxPlayerLevel)
    // so servers running a higher level cap can scale appropriately.
    if (maxPlayerLevel == 0) maxPlayerLevel = 80;
    uint8 ceiling = static_cast<uint8>(maxPlayerLevel + zoneTier);
    uint8 floor = zoneLevelMin > 0 ? zoneLevelMin : 1;
    if (highestOnlineInZone < floor)
        return floor;
    if (highestOnlineInZone > ceiling)
        return ceiling;
    return highestOnlineInZone;
}

uint8 ApplyScaling(uint8 baseline, uint8 target)
{
    if (target == 0 || target <= baseline)
        return baseline;
    return target;
}

uint8 AggregatePlayerLevel(std::vector<uint8> levels, bool useMax)
{
    if (levels.empty())
        return 0;
    if (useMax)
        return *std::max_element(levels.begin(), levels.end());
    // Median. For an even count we take the upper of the two middle
    // values (levels[n/2]) so a level-split party leans slightly toward
    // the higher players rather than under-leveling the top half — a
    // 30/72 duo targets 72, not 30. Configurable to straight max via
    // TerrorZones.Scaling.PlayerLevelMetric.
    std::sort(levels.begin(), levels.end());
    return levels[levels.size() / 2];
}

// -----------------------------------------------------------------------------
// Slice 3 — baseline reward math (pure)
// -----------------------------------------------------------------------------

uint32 ComputeMultipliedValue(uint32 baseline, float mult)
{
    if (baseline == 0 || mult <= 0.0f)
        return 0;
    if (mult == 1.0f)
        return baseline;
    double scaled = static_cast<double>(baseline) * static_cast<double>(mult);
    if (scaled >= static_cast<double>(std::numeric_limits<uint32>::max()))
        return std::numeric_limits<uint32>::max();
    if (scaled < 0.0)
        return 0;
    return static_cast<uint32>(scaled);
}

// -----------------------------------------------------------------------------
// Slice 4 — pure flavor helpers
// -----------------------------------------------------------------------------

Flavor SelectFlavor(uint32 const (&weights)[FLAVOR_MAX], IRng& rng)
{
    uint64 total = 0;
    for (uint32 i = 0; i < FLAVOR_MAX; ++i)
        total += weights[i];
    if (total == 0)
        return FLAVOR_NONE;

    if (total <= 1'000'000)
    {
        uint32 roll = rng.NextUInt(static_cast<uint32>(total));
        uint64 acc = 0;
        for (uint32 i = 0; i < FLAVOR_MAX; ++i)
        {
            acc += weights[i];
            if (roll < acc)
                return static_cast<Flavor>(i + 1);
        }
        return static_cast<Flavor>(FLAVOR_MAX);
    }

    constexpr uint32 RESOLUTION = 1'000'000;
    uint32 roll = rng.NextUInt(RESOLUTION);
    double target = (static_cast<double>(roll) / RESOLUTION)
                  * static_cast<double>(total);
    double acc = 0.0;
    for (uint32 i = 0; i < FLAVOR_MAX; ++i)
    {
        acc += static_cast<double>(weights[i]);
        if (target < acc)
            return static_cast<Flavor>(i + 1);
    }
    return static_cast<Flavor>(FLAVOR_MAX);
}

char const* FlavorDisplayName(Flavor flavor)
{
    switch (flavor)
    {
        case FLAVOR_BLOODBATH:   return "Bloodbath";
        case FLAVOR_PROSPECTORS: return "Prospector's";
        case FLAVOR_WARLORDS:    return "Warlord's";
        case FLAVOR_ARCANE:      return "Arcane";
        case FLAVOR_MERCHANTS:   return "Merchant's";
        case FLAVOR_NONE:        return "—";
        default:                 return "—";
    }
}

char const* FlavorCommandKey(Flavor flavor)
{
    switch (flavor)
    {
        case FLAVOR_BLOODBATH:   return "bloodbath";
        case FLAVOR_PROSPECTORS: return "prospectors";
        case FLAVOR_WARLORDS:    return "warlords";
        case FLAVOR_ARCANE:      return "arcane";
        case FLAVOR_MERCHANTS:   return "merchants";
        default:                 return "none";
    }
}

bool IsGatheringStore(char const* storeName)
{
    if (!storeName || !*storeName)
        return false;
    // Herbs + mines both flow through gameobject_loot_template (the
    // gameobject is the node). Skinning has its own store. Fishing is
    // intentionally excluded per plan §9.1 — it has its own rhythm and
    // revisit is a playtest decision, not a Slice 4 one.
    if (std::strcmp(storeName, "gameobject_loot_template") == 0)
        return true;
    if (std::strcmp(storeName, "skinning_loot_template") == 0)
        return true;
    return false;
}

// -----------------------------------------------------------------------------
// Slice 5 — tier + axis-roll math (pure)
// -----------------------------------------------------------------------------

namespace
{
    // SplitMix64 — deterministic finalizer. Produces identical output for
    // identical input across platforms/compilers.
    uint64 Mix64(uint64 x)
    {
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    }

    // Fold the tuple into a single 64-bit seed. The per-component offsets
    // guarantee distinct hashes even when a later component is zero.
    uint64 AxisRollSeed(uint64 tickAt, uint32 slotIndex,
                        Flavor flavor, Tier tier, RewardAxis axis)
    {
        uint64 h = Mix64(tickAt + 0xA);
        h ^= Mix64(static_cast<uint64>(slotIndex) + 0xB100);
        h ^= Mix64(static_cast<uint64>(flavor)    + 0xC20000);
        h ^= Mix64(static_cast<uint64>(tier)      + 0xD3000000);
        h ^= Mix64(static_cast<uint64>(axis)      + 0xE400000000ULL);
        return Mix64(h);
    }

    // Per-flavor primary + secondaries table. Hardcoded per plan §2.8 —
    // flavor identity is design, not tuning. Magnitudes of the bumps are
    // config (TierRollConfig.signature*/secondaryFloorBump). AXIS_COUNT
    // acts as a sentinel for FLAVOR_NONE so queries for a slot that has
    // no flavor resolve to "no primary, no secondaries".
    constexpr FlavorBiasDef kFlavorBiases[FLAVOR_MAX + 1] = {
        // FLAVOR_NONE
        { AXIS_COUNT, AXIS_COUNT, AXIS_COUNT },
        // FLAVOR_BLOODBATH
        { AXIS_XP,        AXIS_GOLD,      AXIS_TIER_BUMP },
        // FLAVOR_PROSPECTORS
        { AXIS_GATHERING, AXIS_UNIQUES,   AXIS_GOLD      },
        // FLAVOR_WARLORDS
        { AXIS_TIER_BUMP, AXIS_UNIQUES,   AXIS_XP        },
        // FLAVOR_ARCANE
        { AXIS_UNIQUES,   AXIS_XP,        AXIS_GATHERING },
        // FLAVOR_MERCHANTS
        { AXIS_GOLD,      AXIS_UNIQUES,   AXIS_TIER_BUMP },
    };
}

Tier SelectTier(uint32 const (&weights)[TIER_MAX], IRng& rng)
{
    uint64 total = 0;
    for (uint32 i = 0; i < TIER_MAX; ++i)
        total += weights[i];
    if (total == 0)
        return TIER_NONE;

    if (total <= 1'000'000)
    {
        uint32 roll = rng.NextUInt(static_cast<uint32>(total));
        uint64 acc = 0;
        for (uint32 i = 0; i < TIER_MAX; ++i)
        {
            acc += weights[i];
            if (roll < acc)
                return static_cast<Tier>(i + 1);
        }
        return static_cast<Tier>(TIER_MAX);
    }

    constexpr uint32 RESOLUTION = 1'000'000;
    uint32 roll = rng.NextUInt(RESOLUTION);
    double target = (static_cast<double>(roll) / RESOLUTION)
                  * static_cast<double>(total);
    double acc = 0.0;
    for (uint32 i = 0; i < TIER_MAX; ++i)
    {
        acc += static_cast<double>(weights[i]);
        if (target < acc)
            return static_cast<Tier>(i + 1);
    }
    return static_cast<Tier>(TIER_MAX);
}

char const* TierDisplayName(Tier tier)
{
    switch (tier)
    {
        case TIER_1: return "Tier 1";
        case TIER_2: return "Tier 2";
        case TIER_3: return "Tier 3";
        case TIER_4: return "Tier 4";
        case TIER_5: return "Tier 5";
        case TIER_NONE:
        default:     return "Tier 1";
    }
}

char const* AxisShortName(RewardAxis axis)
{
    switch (axis)
    {
        case AXIS_XP:        return "XP";
        case AXIS_GOLD:      return "gold";
        case AXIS_TIER_BUMP: return "tier-bump";
        case AXIS_GATHERING: return "gathering";
        case AXIS_UNIQUES:   return "uniques";
        default:             return "?";
    }
}

bool IsProbabilityAxis(RewardAxis axis)
{
    return axis == AXIS_TIER_BUMP || axis == AXIS_UNIQUES;
}

FlavorBiasDef const& FlavorBiasOf(Flavor flavor)
{
    if (flavor == FLAVOR_NONE || flavor > FLAVOR_MAX)
        return kFlavorBiases[FLAVOR_NONE];
    return kFlavorBiases[flavor];
}

float ComputeAxisRoll(
    uint64 tickAt,
    uint32 slotIndex,
    Flavor flavor,
    Tier tier,
    RewardAxis axis,
    TierRollConfig const& cfg)
{
    if (axis >= AXIS_COUNT)
        return 0.0f;

    // §4.5 read-time compat: TIER_NONE (pre-Slice-5 rows) is treated as
    // TIER_1 for math purposes.
    Tier effectiveTier = (tier == TIER_NONE) ? TIER_1 : tier;
    if (effectiveTier > TIER_MAX)
        effectiveTier = TIER_MAX;

    TierAxisBracket const& bracket =
        cfg.tierTable[effectiveTier - 1][axis];
    float base   = bracket.base;
    float spread = bracket.spread;
    if (base <= 0.0f)
        return 0.0f;

    FlavorBiasDef const& bias = FlavorBiasOf(flavor);
    float lo = 0.0f;
    float hi = 0.0f;
    if (bias.primary == axis)
    {
        lo = base * (1.0f + cfg.signatureFloorBump - spread);
        hi = base * (1.0f + cfg.signatureCeilingBump + spread);
    }
    else if (bias.secondaryA == axis || bias.secondaryB == axis)
    {
        lo = base * (1.0f + cfg.secondaryFloorBump - spread);
        hi = base * (1.0f + spread);
    }
    else
    {
        lo = base * (1.0f - spread);
        hi = base * (1.0f + spread);
    }
    if (lo < 0.0f) lo = 0.0f;
    if (hi < lo)   hi = lo;

    uint64 seed = AxisRollSeed(tickAt, slotIndex, flavor,
                                effectiveTier, axis);
    // Draw a 24-bit fraction from the seed — plenty of resolution for
    // the reward curve, cheap and deterministic across platforms.
    float t = static_cast<float>(seed & 0xFFFFFF)
            / static_cast<float>(0x1000000);
    float rolled = lo + (hi - lo) * t;

    // Hard cap against runaway configs (§2.7).
    float cap = cfg.axisCaps[axis];
    if (cap > 0.0f && rolled > cap)
        rolled = cap;
    return rolled;
}

// -----------------------------------------------------------------------------
// Slice 8 — combat-difficulty mult composition (pure)
// -----------------------------------------------------------------------------

namespace
{
    float ComputeCombatMult(float baseline,
                            Tier tier,
                            float const (&tierBonus)[TIER_MAX + 1],
                            bool isEventBoss,
                            float eventBossUplift,
                            bool isPromotedElite,
                            float eliteUplift)
    {
        float m = baseline;
        if (tier != TIER_NONE && tier <= TIER_MAX)
            m *= tierBonus[tier];
        if (isPromotedElite)
            m *= eliteUplift;
        if (isEventBoss)
            m *= eventBossUplift;
        return m < 1.0f ? 1.0f : m;
    }
}

float ComputeCombatHpMult(float baseline,
                          Tier tier,
                          float const (&tierBonus)[TIER_MAX + 1],
                          bool isEventBoss,
                          float eventBossUplift,
                          bool isPromotedElite,
                          float eliteUplift)
{
    return ComputeCombatMult(baseline, tier, tierBonus,
                              isEventBoss, eventBossUplift,
                              isPromotedElite, eliteUplift);
}

float ComputeCombatDamageMult(float baseline,
                              Tier tier,
                              float const (&tierBonus)[TIER_MAX + 1],
                              bool isEventBoss,
                              float eventBossUplift,
                              bool isPromotedElite,
                              float eliteUplift)
{
    return ComputeCombatMult(baseline, tier, tierBonus,
                              isEventBoss, eventBossUplift,
                              isPromotedElite, eliteUplift);
}

// Slice 8b — deterministic elite-promotion decision. Mix the spawn's
// raw ObjectGuid value with the rotation's tickAt (so a new rotation
// promotes a different subset of mobs), then take the result mod 1000
// and compare against the per-mille threshold the caller resolved per
// tier. Threshold 0 → never promote (T1/T2 default); threshold 1000 →
// always promote.
bool IsPromotedSpawn(uint64 rawGuid,
                     uint64 tickAt,
                     uint32 thresholdPerMille)
{
    if (thresholdPerMille == 0)
        return false;
    if (thresholdPerMille >= 1000)
        return true;
    uint64 seed = Mix64(rawGuid ^ Mix64(tickAt + 0x1234567890ABCDEFULL));
    uint32 roll = static_cast<uint32>(seed % 1000ULL);
    return roll < thresholdPerMille;
}

// -----------------------------------------------------------------------------
// Slice 7 — announcement gating + lead-time predicates (pure)
// -----------------------------------------------------------------------------

bool IsCategoryAnnouncementAllowed(AnnounceCategory cat,
                                    uint8 globalMask,
                                    bool playerMasterOn,
                                    uint8 playerMask)
{
    if (!playerMasterOn)
        return false;
    if (cat >= ANNOUNCE_CATEGORY_COUNT)
        return false;
    uint8 bit = AnnounceCategoryBit(cat);
    if (!(globalMask & bit))
        return false;
    if (!(playerMask & bit))
        return false;
    return true;
}

bool ShouldFireRotationEndingWarning(uint64 now,
                                      uint64 nextTickAt,
                                      uint32 leadSec,
                                      uint32 windowSec,
                                      uint64 lastWarnTickAt)
{
    if (leadSec == 0)
        return false;
    if (lastWarnTickAt >= nextTickAt)
        return false;
    if (nextTickAt < leadSec)
        return false;
    uint64 windowStart = nextTickAt - leadSec;
    if (now < windowStart)
        return false;
    if (now > windowStart + windowSec)
        return false;
    return true;
}

bool ShouldFireEventEndingCountdown(uint64 now,
                                     uint64 endsAt,
                                     uint32 leadSec,
                                     uint32 windowSec,
                                     bool   alreadyFired)
{
    if (leadSec == 0 || alreadyFired)
        return false;
    if (endsAt < leadSec)
        return false;
    uint64 windowStart = endsAt - leadSec;
    if (now < windowStart)
        return false;
    if (now > windowStart + windowSec)
        return false;
    return true;
}

namespace
{
    struct AnnounceCategoryNames
    {
        AnnounceCategory cat;
        char const* display;
        char const* key;
    };

    constexpr AnnounceCategoryNames kCategoryNames[] = {
        { ANNOUNCE_ROTATION_TICK,   "Rotation start",       "rotation"        },
        { ANNOUNCE_ROTATION_ENDING, "Rotation ending warn", "rotation-ending" },
        { ANNOUNCE_ROTATION_END,    "Rotation end",         "rotation-end"    },
        { ANNOUNCE_ZONE_ENTRY,      "Zone entry",           "entry"           },
        { ANNOUNCE_ZONE_LEAVE,      "Zone leave",           "leave"           },
        { ANNOUNCE_EVENT_START,     "Event start",          "event-start"     },
        { ANNOUNCE_EVENT_ENDING,    "Event ending warn",    "event-ending"    },
        { ANNOUNCE_EVENT_END,       "Event end",            "event-end"       },
    };
}

char const* AnnounceCategoryDisplayName(AnnounceCategory cat)
{
    for (auto const& e : kCategoryNames)
        if (e.cat == cat)
            return e.display;
    return "Unknown";
}

char const* AnnounceCategoryCommandKey(AnnounceCategory cat)
{
    for (auto const& e : kCategoryNames)
        if (e.cat == cat)
            return e.key;
    return "unknown";
}

AnnounceCategory ParseAnnounceCategoryKey(char const* key)
{
    if (!key)
        return ANNOUNCE_CATEGORY_COUNT;
    for (auto const& e : kCategoryNames)
        if (std::strcmp(e.key, key) == 0)
            return e.cat;
    return ANNOUNCE_CATEGORY_COUNT;
}

uint8 ParseAnnounceCategoryAlias(char const* key)
{
    if (!key)
        return 0;
    if (std::strcmp(key, "all") == 0)
        return ANNOUNCE_CATEGORY_ALL;
    if (std::strcmp(key, "event") == 0 || std::strcmp(key, "events") == 0)
        return AnnounceCategoryBit(ANNOUNCE_EVENT_START)
             | AnnounceCategoryBit(ANNOUNCE_EVENT_ENDING)
             | AnnounceCategoryBit(ANNOUNCE_EVENT_END);
    if (std::strcmp(key, "rotation-all") == 0)
        return AnnounceCategoryBit(ANNOUNCE_ROTATION_TICK)
             | AnnounceCategoryBit(ANNOUNCE_ROTATION_ENDING)
             | AnnounceCategoryBit(ANNOUNCE_ROTATION_END);
    if (std::strcmp(key, "zone") == 0)
        return AnnounceCategoryBit(ANNOUNCE_ZONE_ENTRY)
             | AnnounceCategoryBit(ANNOUNCE_ZONE_LEAVE);
    return 0;
}

// =====================================================================
// Slice 9 Pass 1 — class-targeted drop helpers.
// SLICE_9_PASS_1_CONTENT_PLAN.md §3 (archetype model), §4 (entry
// encoding). Pure functions — no DB, no Mgr state.
// =====================================================================

namespace
{
    // WotLK 3.3.5a class IDs we care about. Listed inline so this
    // helper doesn't take a SharedDefines.h dependency for one byte.
    constexpr uint8 CLASS_WARRIOR     = 1;
    constexpr uint8 CLASS_PALADIN     = 2;
    constexpr uint8 CLASS_HUNTER      = 3;
    constexpr uint8 CLASS_ROGUE       = 4;
    constexpr uint8 CLASS_PRIEST      = 5;
    constexpr uint8 CLASS_DEATHKNIGHT = 6;
    constexpr uint8 CLASS_SHAMAN      = 7;
    constexpr uint8 CLASS_MAGE        = 8;
    constexpr uint8 CLASS_WARLOCK     = 9;
    // No class ID 10.
    constexpr uint8 CLASS_DRUID       = 11;

    constexpr uint32 CLASS_DROP_ENTRY_FLOOR = 700100u;
    // 8 bands × 5 tiers × 5 archetypes × 12 armor slots = 2400 cells.
    constexpr uint32 CLASS_DROP_PER_BAND      = 5u * 5u * 12u; // 300
    constexpr uint32 CLASS_DROP_PER_TIER      = 5u * 12u;      // 60
    constexpr uint32 CLASS_DROP_PER_ARCHETYPE = 12u;
    constexpr uint32 CLASS_DROP_ENTRY_COUNT   = 8u * CLASS_DROP_PER_BAND; // 2400
    constexpr uint32 CLASS_DROP_ENTRY_CEIL    =
        CLASS_DROP_ENTRY_FLOOR + CLASS_DROP_ENTRY_COUNT;       // 702500

    // Weapons live in a parallel range so adding the weapon slot
    // doesn't shift existing armor entry IDs. 8 × 5 × 5 = 200 entries.
    constexpr uint32 WEAPON_DROP_ENTRY_FLOOR  = 703000u;
    constexpr uint32 WEAPON_DROP_PER_BAND     = 5u * 5u;       // 25
    constexpr uint32 WEAPON_DROP_PER_TIER     = 5u;
    constexpr uint32 WEAPON_DROP_ENTRY_COUNT  = 8u * WEAPON_DROP_PER_BAND; // 200
    constexpr uint32 WEAPON_DROP_ENTRY_CEIL   =
        WEAPON_DROP_ENTRY_FLOOR + WEAPON_DROP_ENTRY_COUNT;     // 703200
}

Archetype ArchetypeForClassSpec(uint8 classId, uint8 specIndex)
{
    if (specIndex > 2)
        return ARCHETYPE_NONE;
    switch (classId)
    {
        case CLASS_WARRIOR:
            // 0=Arms, 1=Fury, 2=Protection
            if (specIndex == 2) return ARCHETYPE_TANK;
            return ARCHETYPE_STR_DPS;
        case CLASS_PALADIN:
            // 0=Holy, 1=Protection, 2=Retribution
            if (specIndex == 0) return ARCHETYPE_HEALER;
            if (specIndex == 1) return ARCHETYPE_TANK;
            return ARCHETYPE_STR_DPS;
        case CLASS_HUNTER:
            return ARCHETYPE_AGI_DPS;
        case CLASS_ROGUE:
            return ARCHETYPE_AGI_DPS;
        case CLASS_PRIEST:
            // 0=Discipline, 1=Holy, 2=Shadow
            if (specIndex == 2) return ARCHETYPE_CASTER;
            return ARCHETYPE_HEALER;
        case CLASS_DEATHKNIGHT:
            // 0=Blood (tank per spec), 1=Frost, 2=Unholy
            if (specIndex == 0) return ARCHETYPE_TANK;
            return ARCHETYPE_STR_DPS;
        case CLASS_SHAMAN:
            // 0=Elemental, 1=Enhancement, 2=Restoration
            if (specIndex == 0) return ARCHETYPE_CASTER;
            if (specIndex == 1) return ARCHETYPE_AGI_DPS;
            return ARCHETYPE_HEALER;
        case CLASS_MAGE:
        case CLASS_WARLOCK:
            return ARCHETYPE_CASTER;
        case CLASS_DRUID:
            // 0=Balance, 1=Feral, 2=Restoration
            if (specIndex == 0) return ARCHETYPE_CASTER;
            if (specIndex == 1) return ARCHETYPE_AGI_DPS;
            return ARCHETYPE_HEALER;
        default:
            return ARCHETYPE_NONE;
    }
}

uint32 EncodeClassDropEntry(uint8 bandIndex, Tier tier,
                             Archetype archetype, ArmorSlot slot)
{
    if (bandIndex >= 8)
        return 0;
    if (tier < TIER_1 || tier > TIER_5)
        return 0;
    if (archetype < ARCHETYPE_STR_DPS || archetype > ARCHETYPE_MAX)
        return 0;
    if (slot >= ARMOR_SLOT_COUNT)
        return 0;
    uint32 archIdx = static_cast<uint32>(archetype) - 1u;
    uint32 tierIdx = static_cast<uint32>(tier) - 1u;  // T1=0, T5=4
    if (slot == ARMOR_SLOT_WEAPON)
    {
        return WEAPON_DROP_ENTRY_FLOOR
             + static_cast<uint32>(bandIndex) * WEAPON_DROP_PER_BAND
             + tierIdx * WEAPON_DROP_PER_TIER
             + archIdx;
    }
    return CLASS_DROP_ENTRY_FLOOR
         + static_cast<uint32>(bandIndex) * CLASS_DROP_PER_BAND
         + tierIdx * CLASS_DROP_PER_TIER
         + archIdx * CLASS_DROP_PER_ARCHETYPE
         + static_cast<uint32>(slot);
}

bool DecodeClassDropEntry(uint32 entry,
                           uint8& bandIndex, Tier& tier,
                           Archetype& archetype, ArmorSlot& slot)
{
    if (entry >= WEAPON_DROP_ENTRY_FLOOR
        && entry < WEAPON_DROP_ENTRY_CEIL)
    {
        uint32 rel = entry - WEAPON_DROP_ENTRY_FLOOR;
        uint32 band    = rel / WEAPON_DROP_PER_BAND;
        uint32 tail1   = rel % WEAPON_DROP_PER_BAND;
        uint32 tierIdx = tail1 / WEAPON_DROP_PER_TIER;
        uint32 archIdx = tail1 % WEAPON_DROP_PER_TIER;
        if (band >= 8 || tierIdx >= 5 || archIdx >= 5)
            return false;
        bandIndex = static_cast<uint8>(band);
        tier      = static_cast<Tier>(tierIdx + 1u);
        archetype = static_cast<Archetype>(archIdx + 1u);
        slot      = ARMOR_SLOT_WEAPON;
        return true;
    }
    if (entry < CLASS_DROP_ENTRY_FLOOR || entry >= CLASS_DROP_ENTRY_CEIL)
        return false;
    uint32 rel = entry - CLASS_DROP_ENTRY_FLOOR;
    uint32 band     = rel / CLASS_DROP_PER_BAND;
    uint32 tail1    = rel % CLASS_DROP_PER_BAND;
    uint32 tierIdx  = tail1 / CLASS_DROP_PER_TIER;
    uint32 tail2    = tail1 % CLASS_DROP_PER_TIER;
    uint32 archIdx  = tail2 / CLASS_DROP_PER_ARCHETYPE;
    uint32 slotIdx  = tail2 % CLASS_DROP_PER_ARCHETYPE;
    if (band >= 8 || tierIdx >= 5 || archIdx >= 5 || slotIdx >= 12)
        return false;
    bandIndex = static_cast<uint8>(band);
    tier      = static_cast<Tier>(tierIdx + 1u);  // T1=1, T5=5
    archetype = static_cast<Archetype>(archIdx + 1u);
    slot      = static_cast<ArmorSlot>(slotIdx);
    return true;
}

} // namespace mod_terror_zones
