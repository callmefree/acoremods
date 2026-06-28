// Slice 4 atmosphere + GM test commands + Slice 5 SetActiveTier.
// Everything in here either pushes a weather override onto the live
// maps or mutates the active rotation's flavor/tier state.

#include "TerrorZonesMgr.h"

#include "DatabaseEnv.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "Player.h"
#include "Weather.h"

namespace mod_terror_zones
{

uint32 TerrorZonesMgr::GetFlavorWeatherState(Flavor flavor) const
{
    if (flavor == FLAVOR_NONE || flavor > FLAVOR_MAX)
        return 0;
    return _flavorWeatherState[flavor - 1];
}

float TerrorZonesMgr::GetFlavorWeatherGrade(Flavor flavor) const
{
    if (flavor == FLAVOR_NONE || flavor > FLAVOR_MAX)
        return 0.0f;
    return _flavorWeatherGrade[flavor - 1];
}

namespace
{
    bool MapIsEmpowerable(Map const* map)
    {
        if (!map)
            return false;
        // Instance maps and BGs can't be empowered per spec §11.3, and
        // pushing weather / light overrides into them is wasted work.
        if (map->IsDungeon() || map->IsBattlegroundOrArena())
            return false;
        return true;
    }
}

void TerrorZonesMgr::ApplyAtmosphereOverride(uint32 zoneId, Flavor flavor)
{
    if (!_flavorsEnabled || flavor == FLAVOR_NONE || zoneId == 0)
        return;
    if (!IsWeatherOverrideEnabled())
        return;

    uint32 weatherState = GetFlavorWeatherState(flavor);
    float  weatherGrade = GetFlavorWeatherGrade(flavor);
    if (weatherState == 0)
        return;  // Flavor's weather disabled via state=0.

    sMapMgr->DoForAllMaps([&](Map* map)
    {
        if (!MapIsEmpowerable(map))
            return;
        map->SetZoneWeather(zoneId,
                             static_cast<WeatherState>(weatherState),
                             weatherGrade);
    });

    LOG_INFO("module",
             "mod-terror-zones: atmosphere applied zone={} flavor={} "
             "weather_state={} grade={:.2f}",
             zoneId, FlavorDisplayName(flavor),
             weatherState, weatherGrade);
}

void TerrorZonesMgr::RestoreAtmosphere(uint32 zoneId)
{
    if (zoneId == 0)
        return;

    sMapMgr->DoForAllMaps([&](Map* map)
    {
        if (!MapIsEmpowerable(map))
            return;
        map->SetZoneWeather(zoneId, WEATHER_STATE_FINE, 0.0f);
    });

    LOG_INFO("module",
             "mod-terror-zones: atmosphere restored zone={} (weather=FINE)",
             zoneId);
}

// --- GM test commands (bypass rotation state; plan §5.6) ---

void TerrorZonesMgr::TestApplyWeather(Player* player, uint32 state, float grade)
{
    if (!player)
        return;
    Map* map = player->GetMap();
    if (!map)
        return;
    uint32 zoneId = player->GetZoneId();
    if (zoneId == 0)
        return;
    if (grade < 0.0f) grade = 0.0f;
    if (grade > 1.0f) grade = 1.0f;
    map->SetZoneWeather(zoneId, static_cast<WeatherState>(state), grade);
    LOG_INFO("module",
             "mod-terror-zones: TEST weather applied zone={} state={} grade={:.2f} "
             "by player={}",
             zoneId, state, grade, player->GetGUID().GetCounter());
}

void TerrorZonesMgr::TestApplyFlavor(Player* player, Flavor flavor)
{
    if (!player || flavor == FLAVOR_NONE || flavor > FLAVOR_MAX)
        return;
    Map* map = player->GetMap();
    if (!map)
        return;
    uint32 zoneId = player->GetZoneId();
    if (zoneId == 0)
        return;

    uint32 state = GetFlavorWeatherState(flavor);
    float grade  = GetFlavorWeatherGrade(flavor);
    if (state != 0)
        map->SetZoneWeather(zoneId, static_cast<WeatherState>(state), grade);
    LOG_INFO("module",
             "mod-terror-zones: TEST flavor={} applied zone={} weather_state={} "
             "grade={:.2f} by player={}",
             FlavorDisplayName(flavor), zoneId, state, grade,
             player->GetGUID().GetCounter());
}

void TerrorZonesMgr::TestClearAtmosphere(Player* player)
{
    if (!player)
        return;
    Map* map = player->GetMap();
    if (!map)
        return;
    uint32 zoneId = player->GetZoneId();
    if (zoneId == 0)
        return;
    map->SetZoneWeather(zoneId, WEATHER_STATE_FINE, 0.0f);
    LOG_INFO("module",
             "mod-terror-zones: TEST atmosphere cleared zone={} by player={}",
             zoneId, player->GetGUID().GetCounter());
}

bool TerrorZonesMgr::SetActiveFlavor(Flavor flavor)
{
    if (flavor == FLAVOR_NONE || flavor > FLAVOR_MAX)
        return false;

    if (_rotation.slots.empty())
        return false;
    uint64 tickAt = _rotation.tickAt;
    std::vector<uint32> zoneIds;
    zoneIds.reserve(_rotation.slots.size());
    for (ActiveSlot& s : _rotation.slots)
    {
        s.flavor = flavor;
        zoneIds.push_back(s.zoneId);
    }
    PublishRotationSnap();
    // Flavor doesn't drive combat mults, so no PublishCombatHot needed.

    // Persist flavor change across every slot of this tick.
    CharacterDatabase.Execute(
        "UPDATE terror_zones_history SET flavor = {} WHERE tick_at = {}",
        static_cast<uint32>(flavor), tickAt);

    // Re-apply atmosphere so players in the zone see the new flavor's
    // weather immediately (rewards + gathering + uniques read _rotation
    // live, no further notification needed for those).
    for (uint32 zoneId : zoneIds)
        ApplyAtmosphereOverride(zoneId, flavor);

    LOG_INFO("module",
             "mod-terror-zones: set flavor={} on tick_at={} "
             "({} slot(s)).",
             FlavorDisplayName(flavor), tickAt,
             static_cast<uint32>(zoneIds.size()));
    return true;
}

bool TerrorZonesMgr::SetActiveTier(Tier tier)
{
    if (tier == TIER_NONE || tier > TIER_MAX)
        return false;

    if (_rotation.slots.empty())
        return false;
    uint64 tickAt = _rotation.tickAt;
    for (ActiveSlot& s : _rotation.slots)
        s.tier = tier;
    PublishRotationSnap();
    // Slice 8 — republish combat snapshot so live HP + damage
    // multipliers pick up the tier change immediately without
    // waiting for the next rotation.
    PublishCombatHot();

    // Walk currently-spawned creatures in each rotation zone and
    // re-run SelectLevel so existing mobs pick up the new tier's
    // HP/damage immediately. Force=true bypasses the
    // _scalingRescaleOnTick config gate (this is a GM-driven
    // explicit retune, not the auto-tick edge rescale).
    for (ActiveSlot const& s : _rotation.slots)
        WalkZoneRescale(s.zoneId, /*edgeOn*/ true, /*force*/ true);

    CharacterDatabase.Execute(
        "UPDATE terror_zones_history SET tier = {} WHERE tick_at = {}",
        static_cast<uint32>(tier), tickAt);

    LOG_INFO("module",
             "mod-terror-zones: set tier={} on tick_at={}.",
             TierDisplayName(tier), tickAt);
    return true;
}

} // namespace mod_terror_zones
