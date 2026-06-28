// Slice 1 — rotation lifecycle + announcements + history + player prefs
// + login hooks. Sister files carry the rest:
//   - TerrorZonesMath.cpp     : all pure / free functions (unit-tested)
//   - TerrorZonesScaling.cpp  : Slice 2 creature scaling Mgr methods
//   - TerrorZonesRewards.cpp  : Slice 3/5 XP/gold/tier-bump/gathering/uniques
//   - TerrorZonesAtmosphere.cpp: Slice 4 weather + GM test commands
//                                + SetActiveFlavor / SetActiveTier

#include "TerrorZonesMgr.h"

#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupReference.h"
#include "Log.h"
#include "Player.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <string>
#include <unordered_set>

namespace mod_terror_zones
{

// -----------------------------------------------------------------------------
// Mgr
// -----------------------------------------------------------------------------

TerrorZonesMgr& TerrorZonesMgr::Instance()
{
    static TerrorZonesMgr inst;
    return inst;
}

uint64 TerrorZonesMgr::AlignedBoundary(uint64 now, uint64 intervalSec)
{
    if (intervalSec == 0)
        return now;
    return now - (now % intervalSec);
}

void TerrorZonesMgr::LoadConfig()
{
    _enabled   = sConfigMgr->GetOption<bool>("TerrorZones.Enable", true);
    _debug     = sConfigMgr->GetOption<bool>("TerrorZones.Debug", false);
    _intervalSec = sConfigMgr->GetOption<uint32>(
        "TerrorZones.RotationIntervalSeconds", 3600);
    if (_intervalSec < 60)
        _intervalSec = 60;
    _slotCount = sConfigMgr->GetOption<uint32>("TerrorZones.SlotCount", 1);
    if (_slotCount == 0)
        _slotCount = 1;
    _recencyWindow = sConfigMgr->GetOption<uint32>(
        "TerrorZones.RecencyWindow", 6);
    _recencyMultiplier = sConfigMgr->GetOption<float>(
        "TerrorZones.RecencyDampenMultiplier", 0.1f);
    if (_recencyMultiplier < 0.0)
        _recencyMultiplier = 0.0;
    _levelWindow = sConfigMgr->GetOption<uint32>(
        "TerrorZones.LevelWindow", 5);
    _weightNear    = sConfigMgr->GetOption<uint32>("TerrorZones.Weight.Near", 100);
    _weightOverlap = sConfigMgr->GetOption<uint32>("TerrorZones.Weight.Overlap", 30);
    _weightBelow   = sConfigMgr->GetOption<uint32>("TerrorZones.Weight.Below", 10);
    _weightAbove   = sConfigMgr->GetOption<uint32>("TerrorZones.Weight.Above", 1);
    _announceServerWide = sConfigMgr->GetOption<bool>(
        "TerrorZones.Announce.ServerWide", true);
    _announceStartupTick = sConfigMgr->GetOption<bool>(
        "TerrorZones.Announce.StartupTick", true);
    _announceZoneEntry = sConfigMgr->GetOption<bool>(
        "TerrorZones.Announce.ZoneEntry", true);
    _startupForceTick = sConfigMgr->GetOption<bool>(
        "TerrorZones.StartupForceTick", false);

    // Slice 7 — per-category global mask. Each bit reads its own
    // knob; the legacy `Announce.ServerWide` is OR'd into the
    // RotationTick bit and `Announce.ZoneEntry` is OR'd into the
    // ZoneEntry bit so existing operator configs keep working.
    auto readCategoryBit = [](char const* key, bool dflt) -> bool {
        return sConfigMgr->GetOption<bool>(key, dflt);
    };
    uint8 mask = 0;
    if (readCategoryBit("TerrorZones.Announce.RotationTick", true)
        || _announceServerWide)
        mask |= AnnounceCategoryBit(ANNOUNCE_ROTATION_TICK);
    if (readCategoryBit("TerrorZones.Announce.RotationEnding", true))
        mask |= AnnounceCategoryBit(ANNOUNCE_ROTATION_ENDING);
    if (readCategoryBit("TerrorZones.Announce.RotationEnd", true))
        mask |= AnnounceCategoryBit(ANNOUNCE_ROTATION_END);
    if (readCategoryBit("TerrorZones.Announce.ZoneEntry", true)
        || _announceZoneEntry)
        mask |= AnnounceCategoryBit(ANNOUNCE_ZONE_ENTRY);
    if (readCategoryBit("TerrorZones.Announce.ZoneLeave", true))
        mask |= AnnounceCategoryBit(ANNOUNCE_ZONE_LEAVE);
    if (readCategoryBit("TerrorZones.Announce.EventStart", true))
        mask |= AnnounceCategoryBit(ANNOUNCE_EVENT_START);
    if (readCategoryBit("TerrorZones.Announce.EventEnding", true))
        mask |= AnnounceCategoryBit(ANNOUNCE_EVENT_ENDING);
    if (readCategoryBit("TerrorZones.Announce.EventEnd", true))
        mask |= AnnounceCategoryBit(ANNOUNCE_EVENT_END);
    _announceCategoryGlobal = mask;
    _rotationEndingLeadSec = sConfigMgr->GetOption<uint32>(
        "TerrorZones.Announce.RotationEndingLeadSec", 300);
    _eventEndingLeadSec = sConfigMgr->GetOption<uint32>(
        "TerrorZones.Announce.EventEndingLeadSec", 300);

    _scalingEnabled = sConfigMgr->GetOption<bool>(
        "TerrorZones.Scaling.Enable", true);
    _scalingRescaleOnTick = sConfigMgr->GetOption<bool>(
        "TerrorZones.Scaling.RescaleOnTick", true);
    _maxPlayerLevel = static_cast<uint8>(std::clamp<uint32>(
        sConfigMgr->GetOption<uint32>("TerrorZones.MaxPlayerLevel", 80),
        1u, 255u));
    _scalingSkipWorldBosses = sConfigMgr->GetOption<bool>(
        "TerrorZones.Scaling.SkipWorldBosses", true);
    _scalingSkipFriendly = sConfigMgr->GetOption<bool>(
        "TerrorZones.Scaling.SkipFriendly", false);

    _scalingNeverEntries.clear();
    {
        std::string csv = sConfigMgr->GetOption<std::string>(
            "TerrorZones.Scaling.NeverScaleEntries", "");
        std::stringstream ss(csv);
        std::string token;
        while (std::getline(ss, token, ','))
        {
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())))
                token.erase(token.begin());
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
                token.pop_back();
            if (token.empty())
                continue;
            long v = std::strtol(token.c_str(), nullptr, 10);
            if (v > 0)
                _scalingNeverEntries.insert(static_cast<uint32>(v));
        }
    }

    _rewardsEnabled = sConfigMgr->GetOption<bool>(
        "TerrorZones.Rewards.Enable", true);
    _xpMultiplier = sConfigMgr->GetOption<float>(
        "TerrorZones.Rewards.XpMultiplier", 1.5f);
    if (_xpMultiplier < 0.0f)
        _xpMultiplier = 0.0f;
    _goldMultiplier = sConfigMgr->GetOption<float>(
        "TerrorZones.Rewards.GoldMultiplier", 1.5f);
    if (_goldMultiplier < 0.0f)
        _goldMultiplier = 0.0f;
    _tierBumpChance = sConfigMgr->GetOption<float>(
        "TerrorZones.Rewards.TierBumpChance", 0.03f);
    if (_tierBumpChance < 0.0f) _tierBumpChance = 0.0f;
    if (_tierBumpChance > 1.0f) _tierBumpChance = 1.0f;
    _tierBumpLevelTolerance = sConfigMgr->GetOption<uint32>(
        "TerrorZones.Rewards.TierBumpLevelTolerance", 5);
    _maxBumpQuality = sConfigMgr->GetOption<uint32>(
        "TerrorZones.Rewards.MaxBumpQuality", 4);
    if (_maxBumpQuality > ITEM_QUALITY_EPIC)
        _maxBumpQuality = ITEM_QUALITY_EPIC;
    _goldLevelRatioExp = sConfigMgr->GetOption<float>(
        "TerrorZones.Rewards.GoldLevelRatioExp", 2.0f);
    if (_goldLevelRatioExp < 0.0f)
        _goldLevelRatioExp = 0.0f;

    // --- Slice 4 — flavors ---
    _flavorsEnabled = sConfigMgr->GetOption<bool>(
        "TerrorZones.Flavor.Enable", true);

    _flavorWeights[FLAVOR_BLOODBATH   - 1] = sConfigMgr->GetOption<uint32>("TerrorZones.Flavor.Weight.Bloodbath",   100);
    _flavorWeights[FLAVOR_PROSPECTORS - 1] = sConfigMgr->GetOption<uint32>("TerrorZones.Flavor.Weight.Prospectors", 100);
    _flavorWeights[FLAVOR_WARLORDS    - 1] = sConfigMgr->GetOption<uint32>("TerrorZones.Flavor.Weight.Warlords",    100);
    _flavorWeights[FLAVOR_ARCANE      - 1] = sConfigMgr->GetOption<uint32>("TerrorZones.Flavor.Weight.Arcane",      100);
    _flavorWeights[FLAVOR_MERCHANTS   - 1] = sConfigMgr->GetOption<uint32>("TerrorZones.Flavor.Weight.Merchants",   100);

    auto readOverlay = [&](Flavor f, char const* key, float def, float& out, bool clampNonNeg = true)
    {
        out = sConfigMgr->GetOption<float>(key, def);
        if (clampNonNeg && out < 0.0f)
            out = 0.0f;
        (void)f;
    };
    readOverlay(FLAVOR_BLOODBATH,   "TerrorZones.Flavor.Overlay.Bloodbath.XpBoost",      1.50f, _flavorXpBoost[FLAVOR_BLOODBATH - 1]);
    readOverlay(FLAVOR_BLOODBATH,   "TerrorZones.Flavor.Overlay.Bloodbath.GoldBoost",    1.00f, _flavorGoldBoost[FLAVOR_BLOODBATH - 1]);
    readOverlay(FLAVOR_BLOODBATH,   "TerrorZones.Flavor.Overlay.Bloodbath.TierBumpAdd",  0.00f, _flavorTierBumpAdd[FLAVOR_BLOODBATH - 1]);
    readOverlay(FLAVOR_PROSPECTORS, "TerrorZones.Flavor.Overlay.Prospectors.XpBoost",    1.00f, _flavorXpBoost[FLAVOR_PROSPECTORS - 1]);
    readOverlay(FLAVOR_PROSPECTORS, "TerrorZones.Flavor.Overlay.Prospectors.GoldBoost",  1.25f, _flavorGoldBoost[FLAVOR_PROSPECTORS - 1]);
    readOverlay(FLAVOR_PROSPECTORS, "TerrorZones.Flavor.Overlay.Prospectors.TierBumpAdd", 0.00f, _flavorTierBumpAdd[FLAVOR_PROSPECTORS - 1]);
    readOverlay(FLAVOR_WARLORDS,    "TerrorZones.Flavor.Overlay.Warlords.XpBoost",       1.00f, _flavorXpBoost[FLAVOR_WARLORDS - 1]);
    readOverlay(FLAVOR_WARLORDS,    "TerrorZones.Flavor.Overlay.Warlords.GoldBoost",     1.00f, _flavorGoldBoost[FLAVOR_WARLORDS - 1]);
    readOverlay(FLAVOR_WARLORDS,    "TerrorZones.Flavor.Overlay.Warlords.TierBumpAdd",   0.05f, _flavorTierBumpAdd[FLAVOR_WARLORDS - 1]);
    readOverlay(FLAVOR_ARCANE,      "TerrorZones.Flavor.Overlay.Arcane.XpBoost",         1.25f, _flavorXpBoost[FLAVOR_ARCANE - 1]);
    readOverlay(FLAVOR_ARCANE,      "TerrorZones.Flavor.Overlay.Arcane.GoldBoost",       1.00f, _flavorGoldBoost[FLAVOR_ARCANE - 1]);
    readOverlay(FLAVOR_ARCANE,      "TerrorZones.Flavor.Overlay.Arcane.TierBumpAdd",     0.00f, _flavorTierBumpAdd[FLAVOR_ARCANE - 1]);
    readOverlay(FLAVOR_MERCHANTS,   "TerrorZones.Flavor.Overlay.Merchants.XpBoost",      1.00f, _flavorXpBoost[FLAVOR_MERCHANTS - 1]);
    readOverlay(FLAVOR_MERCHANTS,   "TerrorZones.Flavor.Overlay.Merchants.GoldBoost",    2.00f, _flavorGoldBoost[FLAVOR_MERCHANTS - 1]);
    readOverlay(FLAVOR_MERCHANTS,   "TerrorZones.Flavor.Overlay.Merchants.TierBumpAdd",  0.00f, _flavorTierBumpAdd[FLAVOR_MERCHANTS - 1]);

    _flavorWeatherOverride = sConfigMgr->GetOption<bool>("TerrorZones.Flavor.WeatherOverride.Enable", true);
    _flavorWeatherState[FLAVOR_BLOODBATH - 1]   = sConfigMgr->GetOption<uint32>("TerrorZones.Flavor.Weather.Bloodbath.State",   90);
    _flavorWeatherGrade[FLAVOR_BLOODBATH - 1]   = sConfigMgr->GetOption<float>( "TerrorZones.Flavor.Weather.Bloodbath.Grade",   0.75f);
    _flavorWeatherState[FLAVOR_PROSPECTORS - 1] = sConfigMgr->GetOption<uint32>("TerrorZones.Flavor.Weather.Prospectors.State", 1);
    _flavorWeatherGrade[FLAVOR_PROSPECTORS - 1] = sConfigMgr->GetOption<float>( "TerrorZones.Flavor.Weather.Prospectors.Grade", 0.40f);
    _flavorWeatherState[FLAVOR_WARLORDS - 1]    = sConfigMgr->GetOption<uint32>("TerrorZones.Flavor.Weather.Warlords.State",    86);
    _flavorWeatherGrade[FLAVOR_WARLORDS - 1]    = sConfigMgr->GetOption<float>( "TerrorZones.Flavor.Weather.Warlords.Grade",    0.85f);
    _flavorWeatherState[FLAVOR_ARCANE - 1]      = sConfigMgr->GetOption<uint32>("TerrorZones.Flavor.Weather.Arcane.State",      1);
    _flavorWeatherGrade[FLAVOR_ARCANE - 1]      = sConfigMgr->GetOption<float>( "TerrorZones.Flavor.Weather.Arcane.Grade",      0.70f);
    _flavorWeatherState[FLAVOR_MERCHANTS - 1]   = sConfigMgr->GetOption<uint32>("TerrorZones.Flavor.Weather.Merchants.State",   3);
    _flavorWeatherGrade[FLAVOR_MERCHANTS - 1]   = sConfigMgr->GetOption<float>( "TerrorZones.Flavor.Weather.Merchants.Grade",   0.30f);
    for (uint32 i = 0; i < FLAVOR_MAX; ++i)
    {
        if (_flavorWeatherGrade[i] < 0.0f) _flavorWeatherGrade[i] = 0.0f;
        if (_flavorWeatherGrade[i] > 1.0f) _flavorWeatherGrade[i] = 1.0f;
    }

    _flavorGatheringYieldMult = sConfigMgr->GetOption<float>("TerrorZones.Flavor.Gathering.YieldMultiplier", 2.0f);
    if (_flavorGatheringYieldMult < 1.0f)
        _flavorGatheringYieldMult = 1.0f;
    _flavorGatheringBonusChance = sConfigMgr->GetOption<float>("TerrorZones.Flavor.Gathering.BonusRollChance", 0.50f);
    if (_flavorGatheringBonusChance < 0.0f) _flavorGatheringBonusChance = 0.0f;
    if (_flavorGatheringBonusChance > 1.0f) _flavorGatheringBonusChance = 1.0f;

    _flavorUniquesEnabled  = sConfigMgr->GetOption<bool>( "TerrorZones.Flavor.Uniques.Enable", true);
    _flavorUniquesBaseChance = sConfigMgr->GetOption<float>("TerrorZones.Flavor.Uniques.BaseChance", 0.02f);
    if (_flavorUniquesBaseChance < 0.0f) _flavorUniquesBaseChance = 0.0f;
    if (_flavorUniquesBaseChance > 1.0f) _flavorUniquesBaseChance = 1.0f;
    _flavorUniquesMinMobLevel = sConfigMgr->GetOption<uint32>("TerrorZones.Flavor.Uniques.MinMobLevel", 0);

    // --- Slice 5 — tiers ---
    _tierEnabled = sConfigMgr->GetOption<bool>("TerrorZones.Tier.Enable", true);

    // Tier rarity weights. Default 40/30/20/8/2 makes T5 a ~2% rotation
    // (~1 per day on a 1h cycle). Admins can zero weights to exclude
    // specific tiers (e.g., a "no T5" server sets the T5 weight to 0).
    _tierWeights[0] = sConfigMgr->GetOption<uint32>("TerrorZones.Tier.Rarity.T1", 40);
    _tierWeights[1] = sConfigMgr->GetOption<uint32>("TerrorZones.Tier.Rarity.T2", 30);
    _tierWeights[2] = sConfigMgr->GetOption<uint32>("TerrorZones.Tier.Rarity.T3", 20);
    _tierWeights[3] = sConfigMgr->GetOption<uint32>("TerrorZones.Tier.Rarity.T4", 8);
    _tierWeights[4] = sConfigMgr->GetOption<uint32>("TerrorZones.Tier.Rarity.T5", 2);

    // Per-tier per-axis (base, spread). Defaults mirror
    // TERROR_ZONES_SPEC.md v0.2 §4.4 table.
    struct AxisDefault { float base; float spread; };
    static AxisDefault const kDefaults[TIER_MAX][AXIS_COUNT] = {
        // T1
        {{1.20f, 0.10f}, {1.20f, 0.10f}, {0.025f, 0.25f}, {1.15f, 0.10f}, {0.010f, 0.30f}},
        // T2
        {{1.35f, 0.12f}, {1.35f, 0.12f}, {0.040f, 0.25f}, {1.30f, 0.12f}, {0.020f, 0.30f}},
        // T3
        {{1.55f, 0.15f}, {1.55f, 0.15f}, {0.065f, 0.30f}, {1.50f, 0.15f}, {0.040f, 0.35f}},
        // T4
        {{1.85f, 0.18f}, {1.85f, 0.18f}, {0.095f, 0.30f}, {1.75f, 0.18f}, {0.075f, 0.35f}},
        // T5
        {{2.30f, 0.20f}, {2.30f, 0.20f}, {0.140f, 0.30f}, {2.10f, 0.20f}, {0.140f, 0.40f}},
    };
    static char const* const kTierPrefix[TIER_MAX] = {
        "TerrorZones.Tier.T1.",
        "TerrorZones.Tier.T2.",
        "TerrorZones.Tier.T3.",
        "TerrorZones.Tier.T4.",
        "TerrorZones.Tier.T5.",
    };
    static char const* const kAxisSuffix[AXIS_COUNT] = {
        "Xp.",
        "Gold.",
        "TierBump.",
        "Gathering.",
        "Uniques.",
    };

    for (uint32 t = 0; t < TIER_MAX; ++t)
    {
        for (uint32 a = 0; a < AXIS_COUNT; ++a)
        {
            std::string baseKey = std::string(kTierPrefix[t]) + kAxisSuffix[a] + "Base";
            std::string spreadKey = std::string(kTierPrefix[t]) + kAxisSuffix[a] + "Spread";
            float base = sConfigMgr->GetOption<float>(baseKey,
                kDefaults[t][a].base);
            float spread = sConfigMgr->GetOption<float>(spreadKey,
                kDefaults[t][a].spread);
            if (base < 0.0f)   base = 0.0f;
            if (spread < 0.0f) spread = 0.0f;
            if (spread > 0.99f) spread = 0.99f;  // floor can't go negative
            _tierCfg.tierTable[t][a].base   = base;
            _tierCfg.tierTable[t][a].spread = spread;
        }
    }

    _tierCfg.signatureFloorBump = sConfigMgr->GetOption<float>(
        "TerrorZones.Tier.Bias.SignatureFloorBump", 0.15f);
    _tierCfg.signatureCeilingBump = sConfigMgr->GetOption<float>(
        "TerrorZones.Tier.Bias.SignatureCeilingBump", 0.30f);
    _tierCfg.secondaryFloorBump = sConfigMgr->GetOption<float>(
        "TerrorZones.Tier.Bias.SecondaryFloorBump", 0.05f);

    _tierCfg.axisCaps[AXIS_XP]        = sConfigMgr->GetOption<float>("TerrorZones.Tier.Cap.Xp",        5.0f);
    _tierCfg.axisCaps[AXIS_GOLD]      = sConfigMgr->GetOption<float>("TerrorZones.Tier.Cap.Gold",      8.0f);
    _tierCfg.axisCaps[AXIS_TIER_BUMP] = sConfigMgr->GetOption<float>("TerrorZones.Tier.Cap.TierBump",  0.50f);
    _tierCfg.axisCaps[AXIS_GATHERING] = sConfigMgr->GetOption<float>("TerrorZones.Tier.Cap.Gathering", 4.0f);
    _tierCfg.axisCaps[AXIS_UNIQUES]   = sConfigMgr->GetOption<float>("TerrorZones.Tier.Cap.Uniques",   0.50f);
    for (uint32 a = 0; a < AXIS_COUNT; ++a)
        if (_tierCfg.axisCaps[a] < 0.0f)
            _tierCfg.axisCaps[a] = 0.0f;

    // --- Slice 6 — dynamic events ---
    _eventsEnabled = sConfigMgr->GetOption<bool>(
        "TerrorZones.Events.Enable", true);

    _eventCfg.fireChance = sConfigMgr->GetOption<float>(
        "TerrorZones.Events.FireChance", 0.60f);
    if (_eventCfg.fireChance < 0.0f) _eventCfg.fireChance = 0.0f;
    if (_eventCfg.fireChance > 1.0f) _eventCfg.fireChance = 1.0f;
    _eventCfg.secondChance = sConfigMgr->GetOption<float>(
        "TerrorZones.Events.SecondChance", 0.20f);
    if (_eventCfg.secondChance < 0.0f) _eventCfg.secondChance = 0.0f;
    if (_eventCfg.secondChance > 1.0f) _eventCfg.secondChance = 1.0f;
    _eventCfg.durationSec = sConfigMgr->GetOption<uint32>(
        "TerrorZones.Events.DurationSec", 1200);
    if (_eventCfg.durationSec < 60)
        _eventCfg.durationSec = 60;
    _eventCfg.firstOffsetSec = sConfigMgr->GetOption<uint32>(
        "TerrorZones.Events.FirstOffsetSec", 300);
    _eventCfg.secondOffsetSec = sConfigMgr->GetOption<uint32>(
        "TerrorZones.Events.SecondOffsetSec", 1800);
    _eventRetentionHours = sConfigMgr->GetOption<uint32>(
        "TerrorZones.Events.RetentionHours", 24);

    for (uint32 i = 0; i <= EVENT_TYPE_MAX; ++i)
    {
        _eventCfg.typeWeights[i] = 0;
        _eventCfg.typeEnabled[i] = false;
    }
    _eventCfg.typeWeights[EVENT_WORLD_BOSS] = sConfigMgr->GetOption<uint32>(
        "TerrorZones.Events.Type.WorldBoss.Weight", 100);
    _eventCfg.typeEnabled[EVENT_WORLD_BOSS] = sConfigMgr->GetOption<bool>(
        "TerrorZones.Events.Type.WorldBoss.Enable", true);
    _eventCfg.typeWeights[EVENT_RARE_NODE_SURGE] = sConfigMgr->GetOption<uint32>(
        "TerrorZones.Events.Type.NodeSurge.Weight", 100);
    _eventCfg.typeEnabled[EVENT_RARE_NODE_SURGE] = sConfigMgr->GetOption<bool>(
        "TerrorZones.Events.Type.NodeSurge.Enable", true);
    _eventCfg.typeWeights[EVENT_TREASURE_CARAVAN] = sConfigMgr->GetOption<uint32>(
        "TerrorZones.Events.Type.Caravan.Weight", 0);
    _eventCfg.typeEnabled[EVENT_TREASURE_CARAVAN] = sConfigMgr->GetOption<bool>(
        "TerrorZones.Events.Type.Caravan.Enable", false);
    _eventCfg.typeWeights[EVENT_CHAMPION_GROUNDS] = sConfigMgr->GetOption<uint32>(
        "TerrorZones.Events.Type.Champion.Weight", 0);
    _eventCfg.typeEnabled[EVENT_CHAMPION_GROUNDS] = sConfigMgr->GetOption<bool>(
        "TerrorZones.Events.Type.Champion.Enable", false);

    _eventBossLootBaseChance = sConfigMgr->GetOption<float>(
        "TerrorZones.Events.WorldBoss.LootBaseChance", 1.0f);
    if (_eventBossLootBaseChance < 0.0f) _eventBossLootBaseChance = 0.0f;
    if (_eventBossLootBaseChance > 1.0f) _eventBossLootBaseChance = 1.0f;
    _eventBossScaleMult = sConfigMgr->GetOption<float>(
        "TerrorZones.Events.WorldBoss.ScaleMult", 5.0f);
    if (_eventBossScaleMult < 0.1f) _eventBossScaleMult = 0.1f;
    if (_eventBossScaleMult > 10.0f) _eventBossScaleMult = 10.0f;
    _eventBossBeaconGoId = sConfigMgr->GetOption<uint32>(
        "TerrorZones.Events.WorldBoss.BeaconGameObjectId", 191763);
    _eventBossTrackerSpellId = sConfigMgr->GetOption<uint32>(
        "TerrorZones.Events.WorldBoss.TrackerSpellId", 1130);
    _eventNodeSurgeDefaultRadius = sConfigMgr->GetOption<float>(
        "TerrorZones.Events.NodeSurge.DefaultRadius", 40.0f);
    if (_eventNodeSurgeDefaultRadius < 1.0f)
        _eventNodeSurgeDefaultRadius = 1.0f;
    _eventNodeSurgeDefaultCount = sConfigMgr->GetOption<uint32>(
        "TerrorZones.Events.NodeSurge.DefaultNodeCount", 8);
    if (_eventNodeSurgeDefaultCount == 0)
        _eventNodeSurgeDefaultCount = 1;
    _eventNodeSurgeZIgnore = sConfigMgr->GetOption<float>(
        "TerrorZones.Events.NodeSurge.ZIgnore", 10.0f);
    _eventNodeSurgeBeaconGoId = sConfigMgr->GetOption<uint32>(
        "TerrorZones.Events.NodeSurge.BeaconGameObjectId", 177015);

    // --- Slice 8 — combat difficulty ---
    _combatEnabled = sConfigMgr->GetOption<bool>(
        "TerrorZones.Combat.Enable", true);
    _combatHpMult = sConfigMgr->GetOption<float>(
        "TerrorZones.Combat.HpMult", 2.0f);
    if (_combatHpMult < 0.1f) _combatHpMult = 0.1f;
    _combatDamageMult = sConfigMgr->GetOption<float>(
        "TerrorZones.Combat.DamageMult", 1.3f);
    if (_combatDamageMult < 0.1f) _combatDamageMult = 0.1f;

    // AC env-var translation note: a config key like
    // `TerrorZones.Combat.TierHpBonus.T1` becomes env
    // `AC_TERROR_ZONES_COMBAT_TIER_HP_BONUS_T_1` — single underscore
    // at the letter→digit boundary (`T1` → `T_1`). Matches the
    // Slice-5 tier-bonus keys. See feedback_ac_env_var_translation in
    // the session memory.
    _tierHpBonus[TIER_NONE] = 1.0f;  // sentinel — no tier rolled
    _tierHpBonus[TIER_1] = sConfigMgr->GetOption<float>(
        "TerrorZones.Combat.TierHpBonus.T1", 1.0f);
    _tierHpBonus[TIER_2] = sConfigMgr->GetOption<float>(
        "TerrorZones.Combat.TierHpBonus.T2", 1.25f);
    _tierHpBonus[TIER_3] = sConfigMgr->GetOption<float>(
        "TerrorZones.Combat.TierHpBonus.T3", 1.5f);
    _tierHpBonus[TIER_4] = sConfigMgr->GetOption<float>(
        "TerrorZones.Combat.TierHpBonus.T4", 1.75f);
    _tierHpBonus[TIER_5] = sConfigMgr->GetOption<float>(
        "TerrorZones.Combat.TierHpBonus.T5", 2.0f);

    _tierDamageBonus[TIER_NONE] = 1.0f;
    _tierDamageBonus[TIER_1] = sConfigMgr->GetOption<float>(
        "TerrorZones.Combat.TierDamageBonus.T1", 1.0f);
    _tierDamageBonus[TIER_2] = sConfigMgr->GetOption<float>(
        "TerrorZones.Combat.TierDamageBonus.T2", 1.0f);
    _tierDamageBonus[TIER_3] = sConfigMgr->GetOption<float>(
        "TerrorZones.Combat.TierDamageBonus.T3", 1.0f);
    _tierDamageBonus[TIER_4] = sConfigMgr->GetOption<float>(
        "TerrorZones.Combat.TierDamageBonus.T4", 1.0f);
    _tierDamageBonus[TIER_5] = sConfigMgr->GetOption<float>(
        "TerrorZones.Combat.TierDamageBonus.T5", 1.0f);

    _eventBossHpMultUplift = sConfigMgr->GetOption<float>(
        "TerrorZones.Combat.EventBoss.HpMult", 4.0f);
    if (_eventBossHpMultUplift < 0.1f) _eventBossHpMultUplift = 0.1f;
    _eventBossDamageMultUplift = sConfigMgr->GetOption<float>(
        "TerrorZones.Combat.EventBoss.DamageMult", 1.75f);
    if (_eventBossDamageMultUplift < 0.1f) _eventBossDamageMultUplift = 0.1f;

    _eventBossLootPoolEnabled = sConfigMgr->GetOption<bool>(
        "TerrorZones.Events.WorldBoss.LootPool.Enable", true);
    _eventBossLootPurpleMultiplier = sConfigMgr->GetOption<float>(
        "TerrorZones.Events.WorldBoss.LootPool.PurpleMultiplier", 1.0f);
    if (_eventBossLootPurpleMultiplier < 0.0f)
        _eventBossLootPurpleMultiplier = 0.0f;

    // --- Slice 9 Pass 1 — class-drop chance per tier ---
    auto loadDropChance = [&](char const* key, float def) -> float {
        float raw = sConfigMgr->GetOption<float>(key, def);
        if (raw < 0.0f) return 0.0f;
        if (raw > 1.0f) return 1.0f;
        return raw;
    };
    _classDropChance[TIER_1] = loadDropChance(
        "TerrorZones.Items.DropChance.T1", 0.05f);
    _classDropChance[TIER_2] = loadDropChance(
        "TerrorZones.Items.DropChance.T2", 0.10f);
    _classDropChance[TIER_3] = loadDropChance(
        "TerrorZones.Items.DropChance.T3", 0.20f);
    _classDropChance[TIER_4] = loadDropChance(
        "TerrorZones.Items.DropChance.T4", 0.35f);
    _classDropChance[TIER_5] = loadDropChance(
        "TerrorZones.Items.DropChance.T5", 0.60f);

    // --- Slice 8b — elite density per tier ---
    // Per-tier % of eligible spawns promoted to "elite-feel" (extra
    // HP + damage on top of the Slice 8 base × tier mult). T1/T2
    // default to 0 — only T3+ rotations carry the texture.
    auto loadDensity = [&](char const* key, uint32 def) -> uint32 {
        uint32 raw = sConfigMgr->GetOption<uint32>(key, def);
        if (raw > 1000) raw = 1000;
        return raw;
    };
    _eliteDensityPerMille[TIER_NONE] = 0;
    _eliteDensityPerMille[TIER_1] = loadDensity(
        "TerrorZones.Combat.EliteDensity.T1", 0);
    _eliteDensityPerMille[TIER_2] = loadDensity(
        "TerrorZones.Combat.EliteDensity.T2", 0);
    _eliteDensityPerMille[TIER_3] = loadDensity(
        "TerrorZones.Combat.EliteDensity.T3", 150);
    _eliteDensityPerMille[TIER_4] = loadDensity(
        "TerrorZones.Combat.EliteDensity.T4", 250);
    _eliteDensityPerMille[TIER_5] = loadDensity(
        "TerrorZones.Combat.EliteDensity.T5", 400);
    _eliteHpMultUplift = sConfigMgr->GetOption<float>(
        "TerrorZones.Combat.Elite.HpMult", 1.5f);
    if (_eliteHpMultUplift < 1.0f) _eliteHpMultUplift = 1.0f;
    _eliteDamageMultUplift = sConfigMgr->GetOption<float>(
        "TerrorZones.Combat.Elite.DamageMult", 1.3f);
    if (_eliteDamageMultUplift < 1.0f) _eliteDamageMultUplift = 1.0f;

    LOG_INFO("module",
             "mod-terror-zones: enabled={}, debug={}, interval={}s, slots={}, "
             "recency(window={}, mult={:.3f}), levelWindow={}, "
             "weights(near={}/overlap={}/below={}/above={}), "
             "announce(server={}, startup={}, entry={}), startupForce={}",
             _enabled, _debug, _intervalSec, _slotCount,
             _recencyWindow, _recencyMultiplier,
             _levelWindow,
             _weightNear, _weightOverlap, _weightBelow, _weightAbove,
             _announceServerWide, _announceStartupTick, _announceZoneEntry,
             _startupForceTick);
    LOG_INFO("module",
             "mod-terror-zones: scaling enable={}, rescale_on_tick={}, "
             "skip_bosses={}, skip_friendly={}, never_entries={}",
             _scalingEnabled, _scalingRescaleOnTick,
             _scalingSkipWorldBosses, _scalingSkipFriendly,
             static_cast<uint32>(_scalingNeverEntries.size()));
    LOG_INFO("module",
             "mod-terror-zones: rewards enable={} xp={:.2f} gold={:.2f} "
             "tier_bump_chance={:.3f} tier_bump_level_tolerance={} "
             "max_bump_quality={} gold_level_ratio_exp={:.2f}",
             _rewardsEnabled, _xpMultiplier, _goldMultiplier,
             _tierBumpChance, _tierBumpLevelTolerance, _maxBumpQuality,
             _goldLevelRatioExp);
    LOG_INFO("module",
             "mod-terror-zones: flavor enable={} weather={} uniques={} "
             "weights=[bb={},pr={},wa={},ar={},me={}] "
             "overlays=[bb_xp={:.2f},pr_gold={:.2f},wa_tier_add={:.3f},"
             "ar_xp={:.2f},me_gold={:.2f}] "
             "gathering=[yield_mult={:.2f},bonus_chance={:.2f}] "
             "uniques=[base_chance={:.3f},min_mob_level={}]",
             _flavorsEnabled, _flavorWeatherOverride, _flavorUniquesEnabled,
             _flavorWeights[0], _flavorWeights[1], _flavorWeights[2],
             _flavorWeights[3], _flavorWeights[4],
             _flavorXpBoost[FLAVOR_BLOODBATH - 1],
             _flavorGoldBoost[FLAVOR_PROSPECTORS - 1],
             _flavorTierBumpAdd[FLAVOR_WARLORDS - 1],
             _flavorXpBoost[FLAVOR_ARCANE - 1],
             _flavorGoldBoost[FLAVOR_MERCHANTS - 1],
             _flavorGatheringYieldMult, _flavorGatheringBonusChance,
             _flavorUniquesBaseChance, _flavorUniquesMinMobLevel);
    LOG_INFO("module",
             "mod-terror-zones: tier enable={} "
             "rarity=[t1={},t2={},t3={},t4={},t5={}] "
             "biases=[sig_floor={:.2f},sig_ceil={:.2f},sec_floor={:.2f}] "
             "caps=[xp={:.2f},gold={:.2f},tier_bump={:.2f},"
             "gather={:.2f},uniques={:.2f}] "
             "t3_bases=[xp={:.2f},gold={:.2f},tier_bump={:.3f},"
             "gather={:.2f},uniques={:.3f}]",
             _tierEnabled,
             _tierWeights[0], _tierWeights[1], _tierWeights[2],
             _tierWeights[3], _tierWeights[4],
             _tierCfg.signatureFloorBump, _tierCfg.signatureCeilingBump,
             _tierCfg.secondaryFloorBump,
             _tierCfg.axisCaps[AXIS_XP],
             _tierCfg.axisCaps[AXIS_GOLD],
             _tierCfg.axisCaps[AXIS_TIER_BUMP],
             _tierCfg.axisCaps[AXIS_GATHERING],
             _tierCfg.axisCaps[AXIS_UNIQUES],
             _tierCfg.tierTable[2][AXIS_XP].base,
             _tierCfg.tierTable[2][AXIS_GOLD].base,
             _tierCfg.tierTable[2][AXIS_TIER_BUMP].base,
             _tierCfg.tierTable[2][AXIS_GATHERING].base,
             _tierCfg.tierTable[2][AXIS_UNIQUES].base);
    LOG_INFO("module",
             "mod-terror-zones: events enable={} fire_chance={:.2f} "
             "second_chance={:.2f} duration={}s offsets=[first={},second={}] "
             "retention={}h types=[boss(w={},en={}), nodes(w={},en={}), "
             "caravan(w={},en={}), champion(w={},en={})] "
             "world_boss=[scale={:.2f}x,beacon_go={},tracker_spell={}] "
             "node_surge=[default_radius={:.1f},default_count={},z_ignore={:.1f},"
             "beacon_go={}]",
             _eventsEnabled,
             _eventCfg.fireChance, _eventCfg.secondChance,
             _eventCfg.durationSec,
             _eventCfg.firstOffsetSec, _eventCfg.secondOffsetSec,
             _eventRetentionHours,
             _eventCfg.typeWeights[EVENT_WORLD_BOSS],
             _eventCfg.typeEnabled[EVENT_WORLD_BOSS],
             _eventCfg.typeWeights[EVENT_RARE_NODE_SURGE],
             _eventCfg.typeEnabled[EVENT_RARE_NODE_SURGE],
             _eventCfg.typeWeights[EVENT_TREASURE_CARAVAN],
             _eventCfg.typeEnabled[EVENT_TREASURE_CARAVAN],
             _eventCfg.typeWeights[EVENT_CHAMPION_GROUNDS],
             _eventCfg.typeEnabled[EVENT_CHAMPION_GROUNDS],
             _eventBossScaleMult, _eventBossBeaconGoId,
             _eventBossTrackerSpellId,
             _eventNodeSurgeDefaultRadius,
             _eventNodeSurgeDefaultCount,
             _eventNodeSurgeZIgnore,
             _eventNodeSurgeBeaconGoId);
    LOG_INFO("module",
             "mod-terror-zones: combat enable={} hp_mult={:.2f} "
             "damage_mult={:.2f} "
             "tier_hp_bonus=[T1={:.2f} T2={:.2f} T3={:.2f} T4={:.2f} T5={:.2f}] "
             "event_boss=[hp=x{:.2f} dmg=x{:.2f}] "
             "elite_density_pm=[T1={} T2={} T3={} T4={} T5={}] "
             "elite_uplift=[hp=x{:.2f} dmg=x{:.2f}] "
             "loot_pool enable={} purple_mult={:.2f}",
             _combatEnabled, _combatHpMult, _combatDamageMult,
             _tierHpBonus[TIER_1], _tierHpBonus[TIER_2],
             _tierHpBonus[TIER_3], _tierHpBonus[TIER_4],
             _tierHpBonus[TIER_5],
             _eventBossHpMultUplift, _eventBossDamageMultUplift,
             _eliteDensityPerMille[TIER_1], _eliteDensityPerMille[TIER_2],
             _eliteDensityPerMille[TIER_3], _eliteDensityPerMille[TIER_4],
             _eliteDensityPerMille[TIER_5],
             _eliteHpMultUplift, _eliteDamageMultUplift,
             _eventBossLootPoolEnabled, _eventBossLootPurpleMultiplier);
    auto bit = [&](AnnounceCategory c) {
        return (_announceCategoryGlobal & AnnounceCategoryBit(c)) ? 1 : 0;
    };
    LOG_INFO("module",
             "mod-terror-zones: announce categories=[tick={} r-ending={} "
             "r-end={} z-entry={} z-leave={} e-start={} e-ending={} "
             "e-end={}] leads=[r-ending={}s e-ending={}s]",
             bit(ANNOUNCE_ROTATION_TICK), bit(ANNOUNCE_ROTATION_ENDING),
             bit(ANNOUNCE_ROTATION_END), bit(ANNOUNCE_ZONE_ENTRY),
             bit(ANNOUNCE_ZONE_LEAVE), bit(ANNOUNCE_EVENT_START),
             bit(ANNOUNCE_EVENT_ENDING), bit(ANNOUNCE_EVENT_END),
             _rotationEndingLeadSec, _eventEndingLeadSec);
}

// Slice 8 — rebuild the combat kHz-read snapshot from
// `_rotation.slots` + `_eventBossSpawnIndex`. World-thread-only;
// no synchronization needed against other writers.
void TerrorZonesMgr::PublishCombatHot()
{
    auto snap = std::make_shared<CombatHotState>();
    snap->slots.reserve(_rotation.slots.size());
    for (ActiveSlot const& s : _rotation.slots)
        snap->slots.push_back({s.zoneId, s.tier});
    snap->eventBossGuids.reserve(_eventBossSpawnIndex.size());
    for (auto const& kv : _eventBossSpawnIndex)
        snap->eventBossGuids.insert(kv.first);
    snap->eventBossTiers.reserve(_eventBossTierMap.size());
    for (auto const& kv : _eventBossTierMap)
        snap->eventBossTiers.emplace(kv.first, kv.second);
    snap->tickAt = _rotation.tickAt;
    std::atomic_store_explicit(&_combatHot,
                                std::shared_ptr<CombatHotState const>(snap),
                                std::memory_order_release);
}

// Slice 8b cleanup — publish a fresh immutable copy of the rotation
// for the read paths (IsZoneEmpowered, GetActiveRotation,
// SendTickLineTo, etc.).
void TerrorZonesMgr::PublishRotationSnap()
{
    auto snap = std::make_shared<ActiveRotation>(_rotation);
    std::atomic_store_explicit(&_rotationSnap,
                                std::shared_ptr<ActiveRotation const>(snap),
                                std::memory_order_release);
}

// Slice 8b cleanup — publish a fresh immutable copy of the live
// event list for `.zones event list` and the world-boss tracker
// loop. Vector copy is cheap (≤10 events in flight).
void TerrorZonesMgr::PublishEventsSnap()
{
    auto snap = std::make_shared<std::vector<ActiveEvent>>(_activeEvents);
    std::atomic_store_explicit(
        &_eventsSnap,
        std::shared_ptr<std::vector<ActiveEvent> const>(snap),
        std::memory_order_release);
}

// Slice 8b cleanup — publish a fresh immutable copy of the
// `(rawGuid → (tickAt, eventId))` index. Loot path reads the value
// (not just membership), so this is distinct from
// `_combatHot.eventBossGuids` which only carries membership.
void TerrorZonesMgr::PublishEventBossSpawnSnap()
{
    auto snap = std::make_shared<std::unordered_map<uint64,
                                                     std::pair<uint64, uint32>>>(
        _eventBossSpawnIndex);
    std::atomic_store_explicit(
        &_eventBossSpawnSnap,
        std::shared_ptr<std::unordered_map<uint64,
                                            std::pair<uint64, uint32>> const>(
            snap),
        std::memory_order_release);
}

void TerrorZonesMgr::LoadPool()
{
    _pool.clear();
    _poolIndex.clear();

    QueryResult result = WorldDatabase.Query(
        "SELECT zone_id, display_name, level_min, level_max, enabled "
        "FROM terror_zones_pool");

    if (!result)
    {
        LOG_WARN("module",
                 "mod-terror-zones: terror_zones_pool is empty or missing. "
                 "Slice 1 migration must be applied before this module is useful.");
        return;
    }

    do
    {
        Field* f = result->Fetch();
        PoolEntry e;
        e.zoneId      = f[0].Get<uint32>();
        e.displayName = f[1].Get<std::string>();
        e.levelMin    = f[2].Get<uint16>();
        e.levelMax    = f[3].Get<uint16>();
        e.enabled     = f[4].Get<uint8>() != 0;
        _poolIndex[e.zoneId] = _pool.size();
        _pool.push_back(std::move(e));
    } while (result->NextRow());

    size_t enabled = 0;
    for (auto const& e : _pool) if (e.enabled) ++enabled;
    LOG_INFO("module",
             "mod-terror-zones: loaded {} zones from terror_zones_pool "
             "({} enabled).", _pool.size(), enabled);
}

void TerrorZonesMgr::InitializeOnStartup()
{
    if (!_enabled)
    {
        LOG_INFO("module", "mod-terror-zones: disabled by config; skipping init.");
        return;
    }

    LoadPool();
    BuildRarityIndex();
    BuildUniqueDropIndex();
    LoadEventContent();
    LoadEventBossLootPool();
    LoadClassDropIndex();

    uint64 now = static_cast<uint64>(::time(nullptr));
    uint64 currentBoundary = AlignedBoundary(now, _intervalSec);

    // Try to resume from last history row if it matches the current window.
    uint64 lastTickAt = 0;
    QueryResult r = CharacterDatabase.Query(
        "SELECT tick_at FROM terror_zones_history "
        "ORDER BY tick_at DESC LIMIT 1");
    if (r)
        lastTickAt = r->Fetch()[0].Get<uint64>();

    bool needFreshRotation = _startupForceTick
                          || (lastTickAt != currentBoundary);

    if (!needFreshRotation)
    {
        // Resume existing rotation from DB.
        QueryResult rr = CharacterDatabase.Query(
            "SELECT slot_index, zone_id, flavor, tier FROM terror_zones_history "
            "WHERE tick_at = {} ORDER BY slot_index ASC", lastTickAt);
        if (rr)
        {
            _rotation.slots.clear();
            _rotation.tickAt = lastTickAt;
            _rotation.expiresAt = lastTickAt + _intervalSec;
            do
            {
                Field* f = rr->Fetch();
                uint32 slotIndex = f[0].Get<uint32>();
                uint32 zoneId = f[1].Get<uint32>();
                uint8  flavor = f[2].Get<uint8>();
                uint8  tier   = f[3].Get<uint8>();
                ActiveSlot s;
                s.zoneId = zoneId;
                s.slotIndex = slotIndex;
                auto it = _poolIndex.find(zoneId);
                s.displayName = (it != _poolIndex.end())
                              ? _pool[it->second].displayName
                              : std::to_string(zoneId);
                s.flavor = (flavor <= FLAVOR_MAX)
                         ? static_cast<Flavor>(flavor)
                         : FLAVOR_NONE;
                s.tier = (tier <= TIER_MAX)
                       ? static_cast<Tier>(tier)
                       : TIER_NONE;
                _rotation.slots.push_back(std::move(s));
            } while (rr->NextRow());
            LOG_INFO("module",
                     "mod-terror-zones: resumed rotation from tick_at={} "
                     "({} slot(s)), expires at {}.",
                     lastTickAt,
                     static_cast<uint32>(_rotation.slots.size()),
                     _rotation.expiresAt);
            // Publish snapshots so the startup rescale walk + live
            // combat hooks see the resumed rotation immediately.
            PublishRotationSnap();
            PublishCombatHot();
        }
        else
        {
            needFreshRotation = true;
        }
    }

    // Re-apply atmosphere override on resumed slots. `sMapMgr->DoForAllMaps`
    // is a no-op if no maps are loaded yet (common at startup), in which
    // case the override lands later when the first player enters via
    // SendZoneDynamicInfo — but if this restart happened with players
    // still connected (`.worldserver restart`), we want to nudge the
    // override back in place immediately.
    if (!needFreshRotation && _flavorsEnabled)
    {
        for (ActiveSlot const& s : _rotation.slots)
            if (s.flavor != FLAVOR_NONE)
                ApplyAtmosphereOverride(s.zoneId, s.flavor);
    }

    // Slice 6 — restore active events that outlived the process exit.
    // Runs after atmosphere re-apply so any zone-scoped announcement
    // FireEvent sends lands on players already reconnected.
    LoadActiveEvents();

    if (needFreshRotation)
    {
        // Count online real-player sessions. During normal OnStartup this
        // is always zero (no one has logged in yet), but the helper keeps
        // the check correct if InitializeOnStartup is ever called after
        // the world is already populated (reload scenarios).
        uint32 realPlayersOnline = 0;
        WorldSessionMgr::SessionMap const& sessions =
            sWorldSessionMgr->GetAllSessions();
        for (auto const& kv : sessions)
        {
            WorldSession* session = kv.second;
            if (!session || session->IsBot())
                continue;
            Player* p = session->GetPlayer();
            if (p && p->IsInWorld())
                ++realPlayersOnline;
        }

        if (_startupForceTick || realPlayersOnline > 0)
        {
            RunRotation(currentBoundary, _announceStartupTick);
            _nextTickAt = currentBoundary + _intervalSec;
        }
        else
        {
            // Defer the first rotation until a real player logs in.
            // Rationale: with no online players, SelectZones falls back
            // to flat random over the whole 22-zone pool (targets=0),
            // which defeats the spec §3.1 "weight toward the levels of
            // players currently online" goal. OnPlayerLogin triggers
            // the rotation on the first real (non-bot) login.
            _rotationDeferredForFirstLogin = true;
            _nextTickAt = 0;  // OnUpdate short-circuits on 0
            LOG_INFO("module",
                     "mod-terror-zones: startup rotation deferred — no "
                     "real players online. First real-player login will "
                     "trigger it.");
        }
    }
    else
    {
        _nextTickAt = currentBoundary + _intervalSec;
    }
}

void TerrorZonesMgr::OnUpdate(uint32 diff)
{
    if (!_enabled || _nextTickAt == 0)
        return;

    _tickAccumMs += diff;
    if (_tickAccumMs < 1000)
        return;
    _tickAccumMs = 0;

    uint64 now = static_cast<uint64>(::time(nullptr));

    // Slice 6 — drive event lifecycle on the same 1Hz cadence as
    // rotation. Runs before the rotation check so PENDING events
    // whose startsAt has passed fire on the same wall-clock second
    // the rotation would tick over.
    if (_eventsEnabled)
        TickEvents(now);

    // Slice 7 — rotation-ending warning. Single-shot per rotation
    // via `_lastRotationEndingWarnTickAt`. The pure helper enforces
    // the slack window + missed-window suppress so a restart-resume
    // doesn't late-fire.
    if (ShouldFireRotationEndingWarning(
            now, _nextTickAt, _rotationEndingLeadSec,
            kAnnounceWindowSec, _lastRotationEndingWarnTickAt))
    {
        SendRotationEndingWarning(_nextTickAt);
        _lastRotationEndingWarnTickAt = _nextTickAt;
    }

    if (now < _nextTickAt)
        return;

    // Collapse any missed boundaries into a single rotation — no catch-up.
    uint64 tickAt = AlignedBoundary(now, _intervalSec);
    RunRotation(tickAt, _announceServerWide);
    _nextTickAt = tickAt + _intervalSec;
}

void TerrorZonesMgr::ForceTick()
{
    uint64 now = static_cast<uint64>(::time(nullptr));
    uint64 tickAt = AlignedBoundary(now, _intervalSec);
    // Advance if the current boundary is already represented, to produce
    // visible rotation churn when the GM pokes the button repeatedly.
    if (tickAt == _rotation.tickAt)
        tickAt += _intervalSec;
    RunRotation(tickAt, true);
    _nextTickAt = tickAt + _intervalSec;
}

void TerrorZonesMgr::RunRotation(uint64 tickAt, bool announce)
{
    // Gather targets (lowest-level-in-group, deduped across groups).
    std::vector<uint8> targets;
    {
        std::unordered_set<uint64> seenGroups;
        WorldSessionMgr::SessionMap const& sessions =
            sWorldSessionMgr->GetAllSessions();
        for (auto const& kv : sessions)
        {
            Player* p = kv.second ? kv.second->GetPlayer() : nullptr;
            if (!p || !p->IsInWorld())
                continue;
            Group* g = p->GetGroup();
            if (g)
            {
                uint64 key = static_cast<uint64>(
                    reinterpret_cast<uintptr_t>(g));
                if (!seenGroups.insert(key).second)
                    continue;
                uint8 minLevel = p->GetLevel();
                for (GroupReference* itr = g->GetFirstMember();
                     itr != nullptr; itr = itr->next())
                {
                    Player* m = itr->GetSource();
                    if (m && m->GetLevel() < minLevel)
                        minLevel = m->GetLevel();
                }
                targets.push_back(minLevel);
            }
            else
            {
                targets.push_back(p->GetLevel());
            }
        }
    }

    // Recent zone IDs: last slotCount*recencyWindow rows.
    std::vector<uint32> recent;
    if (_recencyWindow > 0 && _slotCount > 0)
    {
        uint32 limit = _recencyWindow * _slotCount;
        QueryResult r = CharacterDatabase.Query(
            "SELECT zone_id FROM terror_zones_history "
            "ORDER BY tick_at DESC, slot_index ASC LIMIT {}", limit);
        if (r)
        {
            do
            {
                recent.push_back(r->Fetch()[0].Get<uint32>());
            } while (r->NextRow());
        }
    }

    // `_pool` is write-once at LoadPool, so reading it directly on the
    // world thread is safe.
    SelectionConfig cfg {
        _levelWindow, _weightNear, _weightOverlap,
        _weightBelow, _weightAbove, _recencyMultiplier, _slotCount
    };

    uint64 rngSeed = static_cast<uint64>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    rngSeed ^= tickAt * 0x9E3779B97F4A7C15ULL;
    StdRng rng(rngSeed);

    std::vector<uint32> picks =
        SelectZones(_pool, targets, recent, cfg, rng);

    // Roll one flavor per slot, independent of zone selection (plan §2.2).
    std::vector<Flavor> pickFlavors;
    pickFlavors.reserve(picks.size());
    if (_flavorsEnabled)
    {
        for (size_t i = 0; i < picks.size(); ++i)
        {
            Flavor f = SelectFlavor(_flavorWeights, rng);
            if (f == FLAVOR_NONE)
            {
                LOG_WARN("module",
                         "mod-terror-zones: all flavor weights zero, "
                         "falling back to Bloodbath for slot {}.",
                         static_cast<uint32>(i));
                f = FLAVOR_BLOODBATH;
            }
            pickFlavors.push_back(f);
        }
    }
    else
    {
        pickFlavors.assign(picks.size(), FLAVOR_NONE);
    }

    // Slice 5 — roll one tier per slot, independent of zone + flavor.
    std::vector<Tier> pickTiers;
    pickTiers.reserve(picks.size());
    if (_tierEnabled)
    {
        for (size_t i = 0; i < picks.size(); ++i)
        {
            Tier t = SelectTier(_tierWeights, rng);
            if (t == TIER_NONE)
            {
                LOG_WARN("module",
                         "mod-terror-zones: all tier rarity weights zero, "
                         "falling back to Tier 1 for slot {}.",
                         static_cast<uint32>(i));
                t = TIER_1;
            }
            pickTiers.push_back(t);
        }
    }
    else
    {
        // Slice 4 fallback — tier system disabled, every slot reads as
        // Tier 1 for any .zones display but Apply*/Try* use the flat
        // Slice 4 overlays instead of tier rolls.
        pickTiers.assign(picks.size(), TIER_NONE);
    }

    if (picks.empty())
    {
        uint32 enabledCount = 0;
        for (PoolEntry const& z : _pool)
            if (z.enabled) ++enabledCount;
        LOG_WARN("module",
                 "mod-terror-zones: SelectZones returned no picks at tick_at={} "
                 "(pool size={}, enabled={}).",
                 tickAt, static_cast<uint32>(_pool.size()),
                 enabledCount);
        return;
    }

    // Write history synchronously.
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    for (size_t i = 0; i < picks.size(); ++i)
    {
        trans->Append(
            "INSERT INTO terror_zones_history (tick_at, slot_index, zone_id, flavor, tier) "
            "VALUES ({}, {}, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE zone_id = VALUES(zone_id), "
            "flavor = VALUES(flavor), tier = VALUES(tier)",
            tickAt, static_cast<uint32>(i), picks[i],
            static_cast<uint32>(pickFlavors[i]),
            static_cast<uint32>(pickTiers[i]));
    }
    CharacterDatabase.CommitTransaction(trans);

    // Update live rotation. Capture the previous slot set for
    // edge-diff (tick-on / tick-off walks). World-thread-only — no
    // synchronization against other writers.
    std::vector<uint32> prevZones;
    std::vector<std::pair<uint32, std::string>> prevZoneNames;
    prevZones.reserve(_rotation.slots.size());
    prevZoneNames.reserve(_rotation.slots.size());
    for (ActiveSlot const& s : _rotation.slots)
    {
        prevZones.push_back(s.zoneId);
        prevZoneNames.emplace_back(s.zoneId, s.displayName);
    }
    ActiveRotation rot;
    rot.tickAt = tickAt;
    rot.expiresAt = tickAt + _intervalSec;
    for (size_t i = 0; i < picks.size(); ++i)
    {
        uint32 zoneId = picks[i];
        ActiveSlot s;
        s.zoneId = zoneId;
        s.slotIndex = static_cast<uint32>(i);
        auto it = _poolIndex.find(zoneId);
        s.displayName = (it != _poolIndex.end())
                      ? _pool[it->second].displayName
                      : std::to_string(zoneId);
        s.flavor = pickFlavors[i];
        s.tier   = pickTiers[i];
        rot.slots.push_back(std::move(s));
    }
    _rotation = rot;
    // Publish snapshots so the rescale walk below + the combat hot
    // paths see the new rotation without any synchronization.
    PublishRotationSnap();
    PublishCombatHot();

    // Edge-diff walk the creature maps. Must happen AFTER _rotation is
    // updated so ComputeTargetLevel inside OnBeforeCreatureSelectLevel
    // observes the new state. Tick-off first so a slot that left gets
    // baselined before tick-on for a new pick could conflict (can't
    // happen with dedup, but ordering is cheap).
    std::unordered_set<uint32> newSet(picks.begin(), picks.end());
    std::unordered_set<uint32> prevSet(prevZones.begin(), prevZones.end());
    for (auto const& zn : prevZoneNames)
    {
        uint32 z = zn.first;
        if (newSet.count(z))
            continue;
        // Slice 6 — end any active/pending events in a zone that
        // is leaving the rotation. Keeps event lifetime bounded
        // by the zone's empowerment window: if the rotation
        // rotates off, the world boss / node surge despawn with
        // it rather than lingering until their duration expires.
        EndActiveEventsInZone(z);
        WalkZoneRescale(z, /*edgeOn*/ false);
        RestoreAtmosphere(z);
        // Slice 7 — zone-scoped fade line, gated per-player by the
        // RotationEnd category. Fires after the rescale + atmosphere
        // restore so any straggling tick-driven message is the last
        // thing the player sees from this rotation.
        SendRotationEndLineFor(z, zn.second);
    }
    for (size_t i = 0; i < picks.size(); ++i)
    {
        uint32 z = picks[i];
        if (!prevSet.count(z))
        {
            WalkZoneRescale(z, /*edgeOn*/ true);
            ApplyAtmosphereOverride(z, pickFlavors[i]);
        }
    }

    // Slice 6 — schedule dynamic events for the new rotation's slots.
    // Runs after atmosphere so the event's zone already has its new
    // weather if a late-firing FireEvent sends announcements first.
    ScheduleEvents(tickAt, rot.slots);

    {
        std::string slotList;
        for (size_t i = 0; i < rot.slots.size(); ++i)
        {
            if (i) slotList += ", ";
            slotList += rot.slots[i].displayName;
            slotList += "/";
            slotList += TierDisplayName(rot.slots[i].tier);
            slotList += "/";
            slotList += FlavorDisplayName(rot.slots[i].flavor);
        }
        LOG_INFO("module",
                 "mod-terror-zones: rotation tick_at={} slots=[{}] "
                 "targets={} recent={}",
                 tickAt, slotList,
                 static_cast<uint32>(targets.size()),
                 static_cast<uint32>(recent.size()));
    }

    if (announce)
        AnnounceRotation(rot);
}

void TerrorZonesMgr::AnnounceRotation(ActiveRotation const& rot)
{
    uint32 remaining = _intervalSec;  // full window at tick time

    WorldSessionMgr::SessionMap const& sessions =
        sWorldSessionMgr->GetAllSessions();
    for (auto const& kv : sessions)
    {
        Player* p = kv.second ? kv.second->GetPlayer() : nullptr;
        if (!p || !p->IsInWorld())
            continue;
        if (!IsCategoryEnabledFor(p, ANNOUNCE_ROTATION_TICK))
            continue;
        for (ActiveSlot const& s : rot.slots)
            SendTickLineTo(p, s.displayName, remaining);
    }
}

namespace
{
    // Render the signature axis roll for the announcement line.
    // Multiplier axes → "+NN% XP". Probability axes → "NN.N% tier-bump".
    // `value` is the raw rolled axis value (pre-cap-clamp already done).
    std::string FormatSignatureAxisForAnnounce(RewardAxis axis, float value)
    {
        char buf[64];
        if (IsProbabilityAxis(axis))
        {
            std::snprintf(buf, sizeof(buf), "%.1f%% %s",
                          value * 100.0f, AxisShortName(axis));
        }
        else
        {
            int pct = static_cast<int>(std::lround((value - 1.0f) * 100.0f));
            std::snprintf(buf, sizeof(buf), "%+d%% %s",
                          pct, AxisShortName(axis));
        }
        return std::string(buf);
    }

    // Build the "Tier N Flavor (+X% axis)" fragment for an announcement.
    std::string TierFlavorFragment(ActiveSlot const& slot,
                                    TierRollConfig const& cfg,
                                    uint64 tickAt)
    {
        std::string out = TierDisplayName(slot.tier);
        out += ' ';
        out += FlavorDisplayName(slot.flavor);
        FlavorBiasDef const& bias = FlavorBiasOf(slot.flavor);
        if (bias.primary < AXIS_COUNT)
        {
            float rolled = ComputeAxisRoll(tickAt, slot.slotIndex,
                                            slot.flavor, slot.tier,
                                            bias.primary, cfg);
            out += " (";
            out += FormatSignatureAxisForAnnounce(bias.primary, rolled);
            out += ')';
        }
        return out;
    }
}

void TerrorZonesMgr::SendTickLineTo(Player* player,
                                    std::string const& zoneName,
                                    uint32 remainingSec)
{
    if (!player || !player->GetSession())
        return;
    ActiveSlot slotCopy;
    bool have = false;
    uint64 tickAt = 0;
    auto rot = std::atomic_load_explicit(&_rotationSnap,
                                          std::memory_order_acquire);
    if (rot)
    {
        tickAt = rot->tickAt;
        for (ActiveSlot const& s : rot->slots)
            if (s.displayName == zoneName)
            {
                slotCopy = s;
                have = true;
                break;
            }
    }
    if (have && slotCopy.flavor != FLAVOR_NONE)
    {
        std::string frag = TierFlavorFragment(slotCopy, _tierCfg, tickAt);
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cffff8040The winds shift. {} is empowered — {}, "
            "{} remaining.|r",
            zoneName, frag,
            FormatRemaining(remainingSec));
    }
    else
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cffff8040The winds shift. {} is empowered — {} remaining.|r",
            zoneName, FormatRemaining(remainingSec));
}

void TerrorZonesMgr::SendEntryLineTo(Player* player,
                                     std::string const& zoneName,
                                     uint32 remainingSec)
{
    if (!player || !player->GetSession())
        return;
    ActiveSlot slotCopy;
    bool have = false;
    auto rot = std::atomic_load_explicit(&_rotationSnap,
                                          std::memory_order_acquire);
    if (rot)
    {
        for (ActiveSlot const& s : rot->slots)
            if (s.displayName == zoneName)
            {
                slotCopy = s;
                have = true;
                break;
            }
    }
    if (have && slotCopy.flavor != FLAVOR_NONE)
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cffff8040You have entered an empowered zone: {} ({} {}). "
            "{} remaining.|r",
            zoneName, TierDisplayName(slotCopy.tier),
            FlavorDisplayName(slotCopy.flavor),
            FormatRemaining(remainingSec));
    else
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cffff8040You have entered an empowered zone: {}. "
            "{} remaining.|r",
            zoneName, FormatRemaining(remainingSec));
}

void TerrorZonesMgr::SendZoneLeaveLineTo(Player* player,
                                           std::string const& zoneName)
{
    if (!player || !player->GetSession() || zoneName.empty())
        return;
    ChatHandler(player->GetSession()).PSendSysMessage(
        "|cffff8040You have left the empowered zone of {}.|r", zoneName);
}

void TerrorZonesMgr::SendRotationEndLineFor(uint32 zoneId,
                                             std::string const& zoneName)
{
    if (zoneId == 0 || zoneName.empty())
        return;
    char buf[192];
    std::snprintf(buf, sizeof(buf),
        "|cffff8040The winds settle. %s's empowerment has ended.|r",
        zoneName.c_str());
    std::string line(buf);
    // Reuse the zone-scoped broadcast machinery, but apply the
    // per-player category gate inline.
    WorldSessionMgr::SessionMap const& sessions =
        sWorldSessionMgr->GetAllSessions();
    for (auto const& kv : sessions)
    {
        WorldSession* session = kv.second;
        if (!session)
            continue;
        Player* p = session->GetPlayer();
        if (!p || !p->IsInWorld())
            continue;
        if (p->GetZoneId() != zoneId)
            continue;
        if (!IsCategoryEnabledFor(p, ANNOUNCE_ROTATION_END))
            continue;
        ChatHandler(session).PSendSysMessage("{}", line);
    }
}

void TerrorZonesMgr::SendRotationEndingWarning(uint64 nextTickAt)
{
    uint64 now = static_cast<uint64>(::time(nullptr));
    uint32 leadSec = (nextTickAt > now)
        ? static_cast<uint32>(nextTickAt - now) : 0;
    char buf[192];
    if (leadSec >= 60)
    {
        uint32 mins = (leadSec + 30) / 60;
        std::snprintf(buf, sizeof(buf),
            "|cffff8040In %u minute%s, the winds shift. The current "
            "empowerments fade.|r",
            mins, (mins == 1 ? "" : "s"));
    }
    else
    {
        std::snprintf(buf, sizeof(buf),
            "|cffff8040In %u seconds, the winds shift. The current "
            "empowerments fade.|r",
            leadSec);
    }
    std::string line(buf);
    WorldSessionMgr::SessionMap const& sessions =
        sWorldSessionMgr->GetAllSessions();
    for (auto const& kv : sessions)
    {
        Player* p = kv.second ? kv.second->GetPlayer() : nullptr;
        if (!p || !p->IsInWorld())
            continue;
        if (!IsCategoryEnabledFor(p, ANNOUNCE_ROTATION_ENDING))
            continue;
        ChatHandler(p->GetSession()).PSendSysMessage("{}", line);
    }
}

std::string TerrorZonesMgr::FormatRemaining(uint32 secs) const
{
    if (secs >= 3600)
    {
        uint32 h = secs / 3600;
        uint32 m = (secs % 3600) / 60;
        if (m == 0)
            return (h == 1) ? "1 hour" : std::to_string(h) + " hours";
        return std::to_string(h) + "h " + std::to_string(m) + "m";
    }
    if (secs >= 60)
    {
        uint32 m = secs / 60;
        return (m == 1) ? "1 minute" : std::to_string(m) + " minutes";
    }
    return std::to_string(secs) + "s";
}

uint32 TerrorZonesMgr::RemainingSeconds(uint64 now) const
{
    if (now == 0)
        now = static_cast<uint64>(::time(nullptr));
    auto rot = std::atomic_load_explicit(&_rotationSnap,
                                          std::memory_order_acquire);
    if (!rot || rot->expiresAt <= now)
        return 0;
    return static_cast<uint32>(rot->expiresAt - now);
}

ActiveRotation TerrorZonesMgr::GetActiveRotation() const
{
    auto rot = std::atomic_load_explicit(&_rotationSnap,
                                          std::memory_order_acquire);
    return rot ? *rot : ActiveRotation{};
}

uint64 TerrorZonesMgr::GetNextTickAt() const
{
    return _nextTickAt;
}

std::vector<PoolEntry> TerrorZonesMgr::GetPool() const
{
    // `_pool` is write-once at startup; safe to copy directly.
    return _pool;
}

std::vector<HistoryTick> TerrorZonesMgr::GetHistory(uint32 maxTicks) const
{
    std::vector<HistoryTick> out;
    if (maxTicks == 0)
        return out;

    uint32 limit = maxTicks * std::max<uint32>(_slotCount, 1);
    QueryResult r = CharacterDatabase.Query(
        "SELECT tick_at, slot_index, zone_id, flavor, tier FROM terror_zones_history "
        "ORDER BY tick_at DESC, slot_index ASC LIMIT {}", limit);
    if (!r)
        return out;

    // `_pool` / `_poolIndex` are write-once at startup; safe to read
    // directly while iterating the DB rows.
    uint64 currentTickAt = 0;
    HistoryTick* current = nullptr;
    do
    {
        Field* f = r->Fetch();
        uint64 tickAt = f[0].Get<uint64>();
        uint32 slotIndex = f[1].Get<uint32>();
        uint32 zoneId = f[2].Get<uint32>();
        uint8  flavor = f[3].Get<uint8>();
        uint8  tier   = f[4].Get<uint8>();
        if (tickAt != currentTickAt || !current)
        {
            if (out.size() >= maxTicks)
                break;
            out.push_back({tickAt, {}});
            current = &out.back();
            currentTickAt = tickAt;
        }
        ActiveSlot s;
        s.zoneId = zoneId;
        s.slotIndex = slotIndex;
        auto it = _poolIndex.find(zoneId);
        s.displayName = (it != _poolIndex.end())
                      ? _pool[it->second].displayName
                      : std::to_string(zoneId);
        s.flavor = (flavor <= FLAVOR_MAX)
                 ? static_cast<Flavor>(flavor)
                 : FLAVOR_NONE;
        s.tier = (tier <= TIER_MAX)
               ? static_cast<Tier>(tier)
               : TIER_NONE;
        current->slots.push_back(std::move(s));
    } while (r->NextRow());

    return out;
}

bool TerrorZonesMgr::IsZoneEmpowered(uint32 zoneId,
                                     std::string* outName,
                                     uint32* outRemainingSec) const
{
    auto rot = std::atomic_load_explicit(&_rotationSnap,
                                          std::memory_order_acquire);
    if (!rot)
        return false;
    for (ActiveSlot const& s : rot->slots)
    {
        if (s.zoneId == zoneId)
        {
            if (outName) *outName = s.displayName;
            if (outRemainingSec)
            {
                uint64 now = static_cast<uint64>(::time(nullptr));
                *outRemainingSec = (rot->expiresAt > now)
                    ? static_cast<uint32>(rot->expiresAt - now) : 0;
            }
            return true;
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
// Player prefs
// -----------------------------------------------------------------------------

void TerrorZonesMgr::LoadPlayerPref(Player* player)
{
    if (!player)
        return;
    uint32 guidLow = player->GetGUID().GetCounter();
    bool enabled = _announceServerWide;
    uint8 categories = ANNOUNCE_CATEGORY_ALL;

    QueryResult r = CharacterDatabase.Query(
        "SELECT announce_enabled, announce_categories "
        "FROM character_terror_zones_prefs WHERE guid = {}", guidLow);
    if (r)
    {
        Field* f = r->Fetch();
        enabled = f[0].Get<uint8>() != 0;
        categories = f[1].Get<uint8>();
    }

    PlayerPref& pref = _prefs[guidLow];
    pref.announceEnabled = enabled;
    pref.dirty = false;
    pref.loaded = true;
    pref.announceCategories = categories;
    // Don't reset lastEmpoweredZone* here — login path may have
    // already populated it before this DB round-trip completes.
}

void TerrorZonesMgr::FlushPlayerPref(Player* player)
{
    if (!player)
        return;
    uint32 guidLow = player->GetGUID().GetCounter();

    auto it = _prefs.find(guidLow);
    if (it == _prefs.end() || !it->second.dirty)
        return;
    bool enabled = it->second.announceEnabled;
    uint8 categories = it->second.announceCategories;
    it->second.dirty = false;

    CharacterDatabase.Execute(
        "INSERT INTO character_terror_zones_prefs "
        "  (guid, announce_enabled, announce_categories) "
        "VALUES ({}, {}, {}) "
        "ON DUPLICATE KEY UPDATE "
        "  announce_enabled = VALUES(announce_enabled), "
        "  announce_categories = VALUES(announce_categories)",
        guidLow, enabled ? 1 : 0, static_cast<uint32>(categories));
}

void TerrorZonesMgr::UnloadPlayerPref(ObjectGuid guid)
{
    uint32 guidLow = guid.GetCounter();
    _prefs.erase(guidLow);
}

bool TerrorZonesMgr::IsAnnounceEnabled(Player const* player) const
{
    if (!player)
        return _announceServerWide;
    uint32 guidLow = player->GetGUID().GetCounter();
    auto it = _prefs.find(guidLow);
    if (it == _prefs.end() || !it->second.loaded)
        return _announceServerWide;
    return it->second.announceEnabled;
}

bool TerrorZonesMgr::IsCategoryEnabledFor(Player const* player,
                                            AnnounceCategory cat) const
{
    if (!player)
        return false;
    uint32 guidLow = player->GetGUID().GetCounter();
    bool master;
    uint8 mask;
    auto it = _prefs.find(guidLow);
    if (it == _prefs.end() || !it->second.loaded)
    {
        // Pref row hasn't loaded yet (rare — happens between
        // OnPlayerLogin and LoadPlayerPref completing). Default
        // to the server-wide master + all-on mask so a line
        // in this window doesn't get spuriously suppressed.
        master = _announceServerWide;
        mask = ANNOUNCE_CATEGORY_ALL;
    }
    else
    {
        master = it->second.announceEnabled;
        mask = it->second.announceCategories;
    }
    return IsCategoryAnnouncementAllowed(cat, _announceCategoryGlobal,
                                          master, mask);
}

void TerrorZonesMgr::SetAnnounceEnabled(Player* player, bool enabled)
{
    if (!player)
        return;
    uint32 guidLow = player->GetGUID().GetCounter();
    PlayerPref& pref = _prefs[guidLow];
    if (pref.loaded && pref.announceEnabled == enabled)
        return;
    pref.announceEnabled = enabled;
    pref.dirty = true;
    pref.loaded = true;
}

uint8 TerrorZonesMgr::GetAnnounceCategories(Player const* player) const
{
    if (!player)
        return ANNOUNCE_CATEGORY_ALL;
    uint32 guidLow = player->GetGUID().GetCounter();
    auto it = _prefs.find(guidLow);
    if (it == _prefs.end() || !it->second.loaded)
        return ANNOUNCE_CATEGORY_ALL;
    return it->second.announceCategories;
}

void TerrorZonesMgr::SetAnnounceCategories(Player* player, uint8 mask)
{
    if (!player)
        return;
    uint32 guidLow = player->GetGUID().GetCounter();
    PlayerPref& pref = _prefs[guidLow];
    if (pref.loaded && pref.announceCategories == mask)
        return;
    pref.announceCategories = mask;
    pref.dirty = true;
    pref.loaded = true;
}

void TerrorZonesMgr::OnPlayerLogin(Player* player)
{
    if (!_enabled || !player)
        return;

    // Trigger the deferred startup rotation on the first real-player
    // login. Bots (Playerbot-populated sessions) don't count — we want
    // the rotation to weight toward an actual human's level.
    if (_rotationDeferredForFirstLogin)
    {
        WorldSession* session = player->GetSession();
        if (session && !session->IsBot() && player->IsInWorld())
        {
            _rotationDeferredForFirstLogin = false;
            uint64 now = static_cast<uint64>(::time(nullptr));
            uint64 boundary = AlignedBoundary(now, _intervalSec);
            LOG_INFO("module",
                     "mod-terror-zones: first real-player login, running "
                     "deferred startup rotation at tick_at={}.", boundary);
            RunRotation(boundary, _announceStartupTick);
            _nextTickAt = boundary + _intervalSec;
        }
    }

    LoadPlayerPref(player);
    uint32 zone = player->GetZoneId();
    std::string name;
    uint32 remaining = 0;
    bool empowered = IsZoneEmpowered(zone, &name, &remaining);
    if (empowered)
    {
        // Seed the last-empowered cache so a subsequent zone-leave
        // can fire its line. Done regardless of category gate state
        // — the cache is module-internal bookkeeping.
        uint32 guidLow = player->GetGUID().GetCounter();
        PlayerPref& pref = _prefs[guidLow];
        pref.lastEmpoweredZoneId = zone;
        pref.lastEmpoweredZoneName = name;
    }
    if (!IsCategoryEnabledFor(player, ANNOUNCE_ZONE_ENTRY))
        return;
    if (empowered)
        SendEntryLineTo(player, name, remaining);
}

void TerrorZonesMgr::OnPlayerUpdateZone(Player* player, uint32 newZone)
{
    if (!_enabled || !player)
        return;

    // Snapshot the player's last-empowered zone (if any), then
    // update the cache for the new zone. Per-player prefs are
    // world-thread-only; no synchronization needed.
    std::string oldName;
    uint32 oldZone = 0;
    uint32 guidLow = player->GetGUID().GetCounter();
    auto it = _prefs.find(guidLow);
    if (it != _prefs.end())
    {
        oldZone = it->second.lastEmpoweredZoneId;
        oldName = it->second.lastEmpoweredZoneName;
    }

    std::string newName;
    uint32 remaining = 0;
    bool newEmpowered = IsZoneEmpowered(newZone, &newName, &remaining);

    // Update the cache to reflect the new zone state. Stays empty
    // when newZone isn't empowered so the next entry crossing fires
    // the entry line cleanly.
    PlayerPref& pref = _prefs[guidLow];
    if (newEmpowered)
    {
        pref.lastEmpoweredZoneId = newZone;
        pref.lastEmpoweredZoneName = newName;
    }
    else
    {
        pref.lastEmpoweredZoneId = 0;
        pref.lastEmpoweredZoneName.clear();
    }

    // Leave-line: fired when the player crosses out of an empowered
    // zone (cached) into a different zone, regardless of whether the
    // new zone is empowered. Skip the cross between two empowered
    // slots in the same rotation — the entry line already handles
    // that with full info.
    if (oldZone != 0 && oldZone != newZone)
    {
        if (IsCategoryEnabledFor(player, ANNOUNCE_ZONE_LEAVE))
            SendZoneLeaveLineTo(player, oldName);
    }

    // Entry-line: fired when the player crosses into an empowered
    // zone they weren't already in.
    if (newEmpowered && oldZone != newZone)
    {
        if (IsCategoryEnabledFor(player, ANNOUNCE_ZONE_ENTRY))
            SendEntryLineTo(player, newName, remaining);
    }
}

} // namespace mod_terror_zones
