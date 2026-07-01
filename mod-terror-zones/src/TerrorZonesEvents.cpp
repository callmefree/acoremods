// Slice 6 — dynamic events.
//
// Event framework + two event types (world boss + rare node surge).
// Treasure caravan + champion hunting grounds are reserved in the
// enum + config surface but inert for now (Slice 6b). See
// docs/terror-zones/SLICE_6_PLAN.md for the full plan.
//
// Lifecycle:
//   Rotation tick → ScheduleEvents writes PENDING rows with
//     staggered startsAt.
//   OnUpdate 1Hz → TickEvents flips PENDING → ACTIVE (and spawns
//     entities), then ACTIVE → EXPIRED (and despawns them) when
//     the window closes. EXPIRED rows stay in memory ~60s for
//     `.zones event list` visibility before being pruned.
//   Worldserver boot → LoadActiveEvents reads rows with
//     endsAt > now, rebuilds live state, and re-fires anything
//     that should be live already.
//
// Zero new core patches — every surface rides an existing public
// API (Map::SummonCreature, Map::SummonGameObject, OnUpdate,
// OnBeforeDropAddItem, WorldSessionMgr iteration).

#include "TerrorZonesMgr.h"

#include "Chat.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "Log.h"
#include "LootMgr.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellAuraDefines.h"
#include "TemporarySummon.h"
#include "Util.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <random>
#include <sstream>
#include <string>
#include <utility>

namespace mod_terror_zones
{

// -----------------------------------------------------------------------------
// Pure helpers (unit-tested in TerrorZonesEventTests.cpp)
// -----------------------------------------------------------------------------

EventType SelectEventType(EventScheduleConfig const& cfg, IRng& rng)
{
    uint64 total = 0;
    for (uint32 i = 1; i <= EVENT_TYPE_MAX; ++i)
        if (cfg.typeEnabled[i])
            total += cfg.typeWeights[i];
    if (total == 0)
        return EVENT_NONE;

    constexpr uint32 RESOLUTION = 1'000'000;
    uint32 roll = rng.NextUInt(RESOLUTION);
    double target = (static_cast<double>(roll) / RESOLUTION)
                  * static_cast<double>(total);
    double acc = 0.0;
    for (uint32 i = 1; i <= EVENT_TYPE_MAX; ++i)
    {
        if (!cfg.typeEnabled[i])
            continue;
        acc += static_cast<double>(cfg.typeWeights[i]);
        if (target < acc)
            return static_cast<EventType>(i);
    }
    // Defensive fallback — the loop should always pick.
    for (uint32 i = EVENT_TYPE_MAX; i >= 1; --i)
        if (cfg.typeEnabled[i] && cfg.typeWeights[i] > 0)
            return static_cast<EventType>(i);
    return EVENT_NONE;
}

bool ShouldFireSecondEvent(float chance, IRng& rng)
{
    if (chance <= 0.0f)
        return false;
    if (chance >= 1.0f)
        return true;
    constexpr uint32 RESOLUTION = 1'000'000;
    uint32 roll = rng.NextUInt(RESOLUTION);
    float t = static_cast<float>(roll) / static_cast<float>(RESOLUTION);
    return t < chance;
}

bool WithinEventWindow(uint64 now, uint64 starts, uint64 ends)
{
    if (ends <= starts)
        return false;
    return now >= starts && now < ends;
}

void PickSubregionAnchor(float ax, float ay, float r, IRng& rng,
                         float& outX, float& outY)
{
    if (r <= 0.0f)
    {
        outX = ax;
        outY = ay;
        return;
    }
    constexpr uint32 RESOLUTION = 1'000'000;
    float u1 = static_cast<float>(rng.NextUInt(RESOLUTION))
             / static_cast<float>(RESOLUTION);
    float u2 = static_cast<float>(rng.NextUInt(RESOLUTION))
             / static_cast<float>(RESOLUTION);
    float theta = u1 * 2.0f * static_cast<float>(M_PI);
    float radius = std::sqrt(u2) * r;   // uniform over the disc
    outX = ax + radius * std::cos(theta);
    outY = ay + radius * std::sin(theta);
}

char const* EventTypeDisplayName(EventType type)
{
    switch (type)
    {
        case EVENT_WORLD_BOSS:       return "World Boss";
        case EVENT_TREASURE_CARAVAN: return "Treasure Caravan";
        case EVENT_RARE_NODE_SURGE:  return "Rare Node Surge";
        case EVENT_CHAMPION_GROUNDS: return "Champion Hunting Grounds";
        case EVENT_NONE:
        default:                     return "—";
    }
}

char const* EventTypeCommandKey(EventType type)
{
    switch (type)
    {
        case EVENT_WORLD_BOSS:       return "worldboss";
        case EVENT_TREASURE_CARAVAN: return "caravan";
        case EVENT_RARE_NODE_SURGE:  return "nodes";
        case EVENT_CHAMPION_GROUNDS: return "champion";
        default:                     return "none";
    }
}

EventType ParseEventTypeKey(char const* key)
{
    if (!key || !*key)
        return EVENT_NONE;
    std::string s(key);
    for (char& c : s) c = static_cast<char>(std::tolower(c));
    if (s == "worldboss" || s == "boss")        return EVENT_WORLD_BOSS;
    if (s == "nodes" || s == "nodesurge"
        || s == "node_surge" || s == "surge")   return EVENT_RARE_NODE_SURGE;
    if (s == "caravan" || s == "treasure")      return EVENT_TREASURE_CARAVAN;
    if (s == "champion" || s == "grounds")      return EVENT_CHAMPION_GROUNDS;
    return EVENT_NONE;
}

char const* EventStateDisplayName(EventState state)
{
    switch (state)
    {
        case EVENT_STATE_PENDING: return "pending";
        case EVENT_STATE_ACTIVE:  return "active";
        case EVENT_STATE_EXPIRED: return "expired";
        default:                  return "?";
    }
}

// -----------------------------------------------------------------------------
// Content loading
// -----------------------------------------------------------------------------

namespace
{
    std::vector<uint32> ParseIdCsv(std::string const& s)
    {
        std::vector<uint32> out;
        std::stringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, ','))
        {
            while (!tok.empty()
                   && std::isspace(static_cast<unsigned char>(tok.front())))
                tok.erase(tok.begin());
            while (!tok.empty()
                   && std::isspace(static_cast<unsigned char>(tok.back())))
                tok.pop_back();
            if (tok.empty())
                continue;
            long v = std::strtol(tok.c_str(), nullptr, 10);
            if (v > 0)
                out.push_back(static_cast<uint32>(v));
        }
        return out;
    }
}

void TerrorZonesMgr::LoadEventContent()
{
    _eventBossDefs.clear();
    _eventNodeSurgeDefs.clear();

    QueryResult bossRows = WorldDatabase.Query(
        "SELECT id, zone_id, level_min, level_max, creature_template_id, "
        "anchor_map, anchor_x, anchor_y, anchor_z, anchor_o, display_name, "
        "weight, enabled, tracker_quest_id FROM terror_zones_event_bosses");
    uint32 bossLoaded = 0, bossSkipped = 0;
    if (bossRows)
    {
        do
        {
            Field* f = bossRows->Fetch();
            EventBossDef d;
            d.id                 = f[0].Get<uint32>();
            d.zoneId             = f[1].Get<uint32>();
            d.levelMin           = f[2].Get<uint8>();
            d.levelMax           = f[3].Get<uint8>();
            d.creatureTemplateId = f[4].Get<uint32>();
            d.anchorMap          = f[5].Get<uint32>();
            d.anchorX            = f[6].Get<float>();
            d.anchorY            = f[7].Get<float>();
            d.anchorZ            = f[8].Get<float>();
            d.anchorO            = f[9].Get<float>();
            d.displayName        = f[10].Get<std::string>();
            d.weight             = f[11].Get<uint32>();
            d.enabled            = f[12].Get<uint8>() != 0;
            d.trackerQuestId     = f[13].Get<uint32>();
            if (!d.enabled)
            {
                ++bossSkipped;
                continue;
            }
            if (!sObjectMgr->GetCreatureTemplate(d.creatureTemplateId))
            {
                LOG_WARN("module",
                         "mod-terror-zones: event boss def id={} references "
                         "missing creature_template entry {}, skipping.",
                         d.id, d.creatureTemplateId);
                ++bossSkipped;
                continue;
            }
            _eventBossDefs.push_back(std::move(d));
            ++bossLoaded;
        } while (bossRows->NextRow());
    }

    QueryResult nodeRows = WorldDatabase.Query(
        "SELECT id, zone_id, level_min, level_max, anchor_map, anchor_x, "
        "anchor_y, anchor_z, radius, node_gameobject_ids, node_count, "
        "display_name, weight, enabled FROM terror_zones_event_node_surges");
    uint32 nodeLoaded = 0, nodeSkipped = 0;
    if (nodeRows)
    {
        do
        {
            Field* f = nodeRows->Fetch();
            EventNodeSurgeDef d;
            d.id          = f[0].Get<uint32>();
            d.zoneId      = f[1].Get<uint32>();
            d.levelMin    = f[2].Get<uint8>();
            d.levelMax    = f[3].Get<uint8>();
            d.anchorMap   = f[4].Get<uint32>();
            d.anchorX     = f[5].Get<float>();
            d.anchorY     = f[6].Get<float>();
            d.anchorZ     = f[7].Get<float>();
            d.radius      = f[8].Get<float>();
            std::string csv = f[9].Get<std::string>();
            d.nodeCount   = f[10].Get<uint32>();
            d.displayName = f[11].Get<std::string>();
            d.weight      = f[12].Get<uint32>();
            d.enabled     = f[13].Get<uint8>() != 0;
            if (!d.enabled)
            {
                ++nodeSkipped;
                continue;
            }
            std::vector<uint32> ids = ParseIdCsv(csv);
            // Validate each ID against gameobject_template; drop invalid
            // ones with a warn line so curation typos don't go silent.
            for (auto it = ids.begin(); it != ids.end(); )
            {
                if (!sObjectMgr->GetGameObjectTemplate(*it))
                {
                    LOG_WARN("module",
                             "mod-terror-zones: node surge def id={} has "
                             "missing gameobject_template entry {}, skipping.",
                             d.id, *it);
                    it = ids.erase(it);
                }
                else
                    ++it;
            }
            if (ids.empty())
            {
                LOG_WARN("module",
                         "mod-terror-zones: node surge def id={} has no valid "
                         "gameobject entries, skipping row.", d.id);
                ++nodeSkipped;
                continue;
            }
            d.nodeIds = std::move(ids);
            _eventNodeSurgeDefs.push_back(std::move(d));
            ++nodeLoaded;
        } while (nodeRows->NextRow());
    }

    LOG_INFO("module",
             "mod-terror-zones: event content loaded — bosses={} (skipped {}), "
             "node surges={} (skipped {}).",
             bossLoaded, bossSkipped, nodeLoaded, nodeSkipped);
}

uint32 TerrorZonesMgr::GetEventBossDefCount() const
{
    // Write-once at LoadEventContent; safe to read directly.
    return static_cast<uint32>(_eventBossDefs.size());
}

uint32 TerrorZonesMgr::GetEventNodeSurgeDefCount() const
{
    return static_cast<uint32>(_eventNodeSurgeDefs.size());
}

// -----------------------------------------------------------------------------
// Scheduling
// -----------------------------------------------------------------------------

namespace
{
    // Deterministic seed for the event RNG at rotation tick. Same
    // (tickAt, slotIndex) always produces the same event choice, which
    // makes the restart-resume path read identical events even if the
    // DB rows ever go missing.
    uint64 EventRotationSeed(uint64 tickAt, uint32 slotIndex)
    {
        uint64 h = tickAt ^ 0x9E3779B97F4A7C15ULL;
        h ^= (static_cast<uint64>(slotIndex) + 1) * 0xBF58476D1CE4E5B9ULL;
        h ^= (h >> 27) * 0x94D049BB133111EBULL;
        return h ^ (h >> 31);
    }
}

void TerrorZonesMgr::ScheduleEvents(uint64 tickAt,
                                    std::vector<ActiveSlot> const& slots)
{
    if (!_eventsEnabled || slots.empty())
        return;

    // World-thread-only state — read `_eventCfg`, `_eventBossDefs`,
    // and `_eventNodeSurgeDefs` directly. `ComputeTargetLevel` reads
    // the rotation snapshot, so it's safe to call inline.
    EventScheduleConfig const& cfg = _eventCfg;
    std::vector<EventBossDef> const& bossDefs = _eventBossDefs;
    std::vector<EventNodeSurgeDef> const& nodeDefs = _eventNodeSurgeDefs;

    std::vector<uint8> slotScaledLevel(slots.size(), 0);
    for (size_t i = 0; i < slots.size(); ++i)
    {
        uint8 lvl = ComputeTargetLevel(slots[i].zoneId);
        slotScaledLevel[i] = (lvl == 0) ? 1 : lvl;
    }

    std::vector<ActiveEvent> newEvents;

    for (size_t i = 0; i < slots.size(); ++i)
    {
        ActiveSlot const& slot = slots[i];
        uint8 scaledLevel = slotScaledLevel[i];
        StdRng rng(EventRotationSeed(tickAt,
                                     static_cast<uint32>(slot.slotIndex)));

        auto pickDef = [&](EventType type, uint32 zoneId,
                           uint32& outDefId, uint32& outMapId,
                           float& outX, float& outY, float& outZ,
                           std::string& outName) -> bool
        {
            if (type == EVENT_WORLD_BOSS)
            {
                std::vector<size_t> cands;
                uint64 total = 0;
                for (size_t k = 0; k < bossDefs.size(); ++k)
                {
                    EventBossDef const& d = bossDefs[k];
                    if (d.zoneId != zoneId) continue;
                    if (scaledLevel < d.levelMin) continue;
                    if (d.levelMax > 0 && scaledLevel > d.levelMax) continue;
                    if (d.weight == 0) continue;
                    cands.push_back(k);
                    total += d.weight;
                }
                if (cands.empty() || total == 0)
                    return false;
                uint32 roll = rng.NextUInt(
                    static_cast<uint32>(std::min<uint64>(total, 1'000'000)));
                uint64 acc = 0;
                size_t picked = cands.front();
                for (size_t c : cands)
                {
                    acc += bossDefs[c].weight;
                    if (roll < acc)
                    {
                        picked = c;
                        break;
                    }
                }
                EventBossDef const& d = bossDefs[picked];
                outDefId = d.id;
                outMapId = d.anchorMap;
                outX = d.anchorX;
                outY = d.anchorY;
                outZ = d.anchorZ;
                outName = d.displayName.empty()
                    ? std::string("World Boss") : d.displayName;
                return true;
            }
            if (type == EVENT_RARE_NODE_SURGE)
            {
                std::vector<size_t> cands;
                uint64 total = 0;
                for (size_t k = 0; k < nodeDefs.size(); ++k)
                {
                    EventNodeSurgeDef const& d = nodeDefs[k];
                    if (d.zoneId != zoneId) continue;
                    if (scaledLevel < d.levelMin) continue;
                    if (d.levelMax > 0 && scaledLevel > d.levelMax) continue;
                    if (d.weight == 0) continue;
                    cands.push_back(k);
                    total += d.weight;
                }
                if (cands.empty() || total == 0)
                    return false;
                uint32 roll = rng.NextUInt(
                    static_cast<uint32>(std::min<uint64>(total, 1'000'000)));
                uint64 acc = 0;
                size_t picked = cands.front();
                for (size_t c : cands)
                {
                    acc += nodeDefs[c].weight;
                    if (roll < acc)
                    {
                        picked = c;
                        break;
                    }
                }
                EventNodeSurgeDef const& d = nodeDefs[picked];
                outDefId = d.id;
                outMapId = d.anchorMap;
                outX = d.anchorX;
                outY = d.anchorY;
                outZ = d.anchorZ;
                outName = d.displayName.empty()
                    ? std::string("a rare surge") : d.displayName;
                return true;
            }
            return false;
        };

        uint32 offsets[2] = { cfg.firstOffsetSec, cfg.secondOffsetSec };
        uint32 slotEventId = 0;

        // Guaranteed full-rotation world boss
        // (TerrorZones.Events.WorldBoss.AlwaysSpawn). Bypasses the
        // FireChance gate and the SelectEventType weighted draw: every
        // empowered slot gets a world boss that is up at rotation start
        // and lasts the whole rotation window. The optional second
        // event still rolls below. When the zone has no curated boss
        // content, we log and fall back to the normal probabilistic
        // path so the slot can still get a node surge.
        bool bossScheduled = false;
        if (_eventBossAlwaysSpawn)
        {
            uint32 bDefId = 0, bMapId = 0;
            float bx = 0.0f, by = 0.0f, bz = 0.0f;
            std::string bName;
            if (pickDef(EVENT_WORLD_BOSS, slot.zoneId, bDefId, bMapId,
                        bx, by, bz, bName))
            {
                uint32 trackerQuestId = 0;
                for (EventBossDef const& d : bossDefs)
                    if (d.id == bDefId)
                    {
                        trackerQuestId = d.trackerQuestId;
                        break;
                    }

                ActiveEvent evt{};
                evt.tickAt         = tickAt;
                evt.slotIndex      = slot.slotIndex;
                evt.eventId        = slotEventId++;
                evt.type           = EVENT_WORLD_BOSS;
                evt.state          = EVENT_STATE_PENDING;
                evt.startsAt       = tickAt;                 // up at rotation start
                evt.endsAt         = tickAt + _intervalSec;  // full rotation window
                evt.zoneId         = slot.zoneId;
                evt.mapId          = bMapId;
                evt.definitionId   = bDefId;
                evt.anchorX        = bx;
                evt.anchorY        = by;
                evt.anchorZ        = bz;
                evt.displayName    = std::move(bName);
                evt.trackerQuestId = trackerQuestId;
                newEvents.push_back(std::move(evt));
                bossScheduled = true;
            }
            else
            {
                LOG_WARN("module",
                         "mod-terror-zones: AlwaysSpawn world boss — no "
                         "curated boss content for zone={} (scaledLevel={}, "
                         "tick_at={}, slot={}); no boss this rotation.",
                         slot.zoneId, scaledLevel, tickAt, slot.slotIndex);
            }
        }

        // First-event coin flip — skipped when the guaranteed boss
        // already filled the first event slot (the optional second
        // event still rolls below).
        if (!bossScheduled && !ShouldFireSecondEvent(cfg.fireChance, rng))
            continue;

        uint32 startEvent = bossScheduled ? 1u : 0u;
        for (uint32 e = startEvent; e < 2; ++e)
        {
            if (e == 1 && !ShouldFireSecondEvent(cfg.secondChance, rng))
                break;
            EventType type = SelectEventType(cfg, rng);
            if (type == EVENT_NONE)
                break;

            uint32 defId = 0;
            uint32 mapId = 0;
            float ax = 0.0f, ay = 0.0f, az = 0.0f;
            std::string name;
            if (!pickDef(type, slot.zoneId, defId, mapId, ax, ay, az, name))
            {
                if (_debug)
                    LOG_INFO("module",
                             "mod-terror-zones: event schedule skipped — "
                             "no {} content for zone={} (tick_at={}, slot={})",
                             EventTypeDisplayName(type),
                             slot.zoneId, tickAt, slot.slotIndex);
                continue;
            }

            uint32 trackerQuestId = 0;
            if (type == EVENT_WORLD_BOSS)
                for (EventBossDef const& d : bossDefs)
                    if (d.id == defId) { trackerQuestId = d.trackerQuestId; break; }

            ActiveEvent evt{};
            evt.tickAt         = tickAt;
            evt.slotIndex      = slot.slotIndex;
            evt.eventId        = slotEventId++;
            evt.type           = type;
            evt.state          = EVENT_STATE_PENDING;
            evt.startsAt       = tickAt + offsets[e];
            evt.endsAt         = evt.startsAt + cfg.durationSec;
            evt.zoneId         = slot.zoneId;
            evt.mapId          = mapId;
            evt.definitionId   = defId;
            evt.anchorX        = ax;
            evt.anchorY        = ay;
            evt.anchorZ        = az;
            evt.displayName    = std::move(name);
            evt.trackerQuestId = trackerQuestId;
            newEvents.push_back(std::move(evt));
        }
    }

    if (newEvents.empty())
        return;

    // Persist + register in-memory together.
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    for (ActiveEvent const& e : newEvents)
    {
        trans->Append(
            "INSERT INTO terror_zones_events (tick_at, slot_index, event_id, "
            "event_type, state, starts_at, ends_at, zone_id, map_id, "
            "definition_id, anchor_x, anchor_y, anchor_z, countdown_fired) "
            "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, 0) "
            "ON DUPLICATE KEY UPDATE event_type=VALUES(event_type), "
            "state=VALUES(state), starts_at=VALUES(starts_at), "
            "ends_at=VALUES(ends_at), zone_id=VALUES(zone_id), "
            "map_id=VALUES(map_id), definition_id=VALUES(definition_id), "
            "anchor_x=VALUES(anchor_x), anchor_y=VALUES(anchor_y), "
            "anchor_z=VALUES(anchor_z), countdown_fired=0",
            e.tickAt, e.slotIndex, e.eventId,
            static_cast<uint32>(e.type), static_cast<uint32>(e.state),
            e.startsAt, e.endsAt, e.zoneId, e.mapId, e.definitionId,
            e.anchorX, e.anchorY, e.anchorZ);
    }
    CharacterDatabase.CommitTransaction(trans);

    for (ActiveEvent& e : newEvents)
        _activeEvents.push_back(std::move(e));
    PublishEventsSnap();

    LOG_INFO("module",
             "mod-terror-zones: scheduled {} event(s) for tick_at={} "
             "across {} slot(s).",
             static_cast<uint32>(newEvents.size()), tickAt,
             static_cast<uint32>(slots.size()));

    // Incidental housekeeping — drop expired rows past retention.
    PruneExpiredEventDbRows();
}

void TerrorZonesMgr::LoadActiveEvents()
{
    if (!_eventsEnabled)
        return;

    uint64 now = static_cast<uint64>(::time(nullptr));
    QueryResult r = CharacterDatabase.Query(
        "SELECT tick_at, slot_index, event_id, event_type, state, "
        "starts_at, ends_at, zone_id, map_id, definition_id, "
        "anchor_x, anchor_y, anchor_z, countdown_fired "
        "FROM terror_zones_events "
        "WHERE ends_at > {} ORDER BY starts_at ASC", now);
    if (!r)
        return;

    std::vector<ActiveEvent> resumed;
    do
    {
        Field* f = r->Fetch();
        ActiveEvent e{};
        e.tickAt          = f[0].Get<uint64>();
        e.slotIndex       = f[1].Get<uint32>();
        e.eventId         = f[2].Get<uint32>();
        e.type            = static_cast<EventType>(f[3].Get<uint8>());
        e.state           = EVENT_STATE_PENDING;  // re-fire from scratch
        e.startsAt        = f[5].Get<uint64>();
        e.endsAt          = f[6].Get<uint64>();
        e.zoneId          = f[7].Get<uint32>();
        e.mapId           = f[8].Get<uint32>();
        e.definitionId    = f[9].Get<uint32>();
        e.anchorX         = f[10].Get<float>();
        e.anchorY         = f[11].Get<float>();
        e.anchorZ         = f[12].Get<float>();
        e.countdownFired  = f[13].Get<uint8>() != 0;

        // Regenerate display name + trackerQuestId from the content def
        // when possible; falls through to the type label if the def row
        // has been dropped since.
        std::string displayName;
        if (e.type == EVENT_WORLD_BOSS)
        {
            for (EventBossDef const& d : _eventBossDefs)
                if (d.id == e.definitionId)
                {
                    displayName       = d.displayName;
                    e.trackerQuestId  = d.trackerQuestId;
                    break;
                }
        }
        else if (e.type == EVENT_RARE_NODE_SURGE)
        {
            for (EventNodeSurgeDef const& d : _eventNodeSurgeDefs)
                if (d.id == e.definitionId)
                {
                    displayName = d.displayName;
                    break;
                }
        }
        if (displayName.empty())
            displayName = EventTypeDisplayName(e.type);
        e.displayName = std::move(displayName);

        resumed.push_back(std::move(e));
    } while (r->NextRow());

    for (ActiveEvent& e : resumed)
        _activeEvents.push_back(std::move(e));
    PublishEventsSnap();

    LOG_INFO("module",
             "mod-terror-zones: event resume loaded {} active/pending event(s).",
             static_cast<uint32>(resumed.size()));

    // Do NOT fire events inline at InitializeOnStartup time — at this
    // point the continent maps aren't loaded yet, so
    // `sMapMgr->FindBaseNonInstanceMap` returns null, and the
    // `ChatHandler` path inside `BroadcastZoneLine` has been observed
    // to wedge the main loop's session handshake setup (log goes
    // silent after LogsDir config-read). The 1Hz OnUpdate cadence
    // picks up due events within a second of boot completing — well
    // before a player actually reaches the world — so inline firing
    // is unnecessary. See ENGINEERING_NOTES 2026-04-23 entry.
}

// -----------------------------------------------------------------------------
// Lifecycle tick
// -----------------------------------------------------------------------------

void TerrorZonesMgr::TickEvents(uint64 now)
{
    // World-thread-only — `_activeEvents` and `_eventBossSpawnIndex`
    // are touched only from this method, ScheduleEvents,
    // LoadActiveEvents, FireEventNow, and EndActiveEventsInZone, all
    // of which run on the world thread. Read paths (combat hooks,
    // GetEventsSnapshot, TryEventBossDrop) go through the published
    // snapshots, so we can mutate the underlying state freely here.
    std::vector<std::pair<uint64, uint32>> toFire;
    std::vector<std::pair<uint64, uint32>> toEnd;
    std::vector<size_t> toPrune;
    std::vector<ActiveEvent> toCountdown;

    for (size_t i = 0; i < _activeEvents.size(); ++i)
    {
        ActiveEvent& e = _activeEvents[i];
        if (e.state == EVENT_STATE_PENDING && now >= e.startsAt)
            toFire.push_back({e.tickAt, e.eventId});
        else if (e.state == EVENT_STATE_ACTIVE && now >= e.endsAt)
            toEnd.push_back({e.tickAt, e.eventId});
        else if (e.state == EVENT_STATE_EXPIRED
                 && now >= e.endsAt + 60)
            toPrune.push_back(i);
        if (e.state == EVENT_STATE_ACTIVE
            && ShouldFireEventEndingCountdown(
                now, e.endsAt, _eventEndingLeadSec,
                kAnnounceWindowSec, e.countdownFired))
        {
            e.countdownFired = true;
            toCountdown.push_back(e);
        }
    }

    // Slice 7 — fire any pending event-ending countdown lines.
    // Persist `countdown_fired` so the state survives a restart.
    for (ActiveEvent const& evt : toCountdown)
    {
        uint32 remaining = (evt.endsAt > static_cast<uint64>(::time(nullptr)))
            ? static_cast<uint32>(evt.endsAt
                                  - static_cast<uint64>(::time(nullptr)))
            : 0;
        SendEventEndingCountdown(evt, remaining);
        PersistEventCountdownFired(evt);
    }

    bool spawnIndexChanged = false;
    bool eventsChanged = !toCountdown.empty();

    for (auto const& key : toFire)
    {
        ActiveEvent snapshot;
        bool haveEvt = false;
        for (ActiveEvent const& e : _activeEvents)
            if (e.tickAt == key.first && e.eventId == key.second
                && e.state == EVENT_STATE_PENDING)
            {
                snapshot = e;
                haveEvt = true;
                break;
            }
        if (!haveEvt)
            continue;
        FireEvent(snapshot);   // mutates `snapshot`
        for (ActiveEvent& e : _activeEvents)
            if (e.tickAt == key.first && e.eventId == key.second)
            {
                e.state             = snapshot.state;
                e.spawnedCreatures  = snapshot.spawnedCreatures;
                e.spawnedGameObjects= snapshot.spawnedGameObjects;
                Tier slotTier = TIER_5;
                if (e.slotIndex < _rotation.slots.size())
                    slotTier = _rotation.slots[e.slotIndex].tier;
                if (slotTier == TIER_NONE)
                    slotTier = TIER_5;
                for (ObjectGuid const& g : snapshot.spawnedCreatures)
                {
                    _eventBossSpawnIndex[g.GetRawValue()] =
                        {e.tickAt, e.eventId};
                    _eventBossTierMap[g.GetRawValue()] = slotTier;
                    spawnIndexChanged = true;
                }
                break;
            }
        eventsChanged = true;
    }
    for (auto const& key : toEnd)
    {
        ActiveEvent snapshot;
        bool haveEvt = false;
        for (ActiveEvent const& e : _activeEvents)
            if (e.tickAt == key.first && e.eventId == key.second
                && e.state == EVENT_STATE_ACTIVE)
            {
                snapshot = e;
                haveEvt = true;
                break;
            }
        if (!haveEvt)
            continue;
        EndEvent(snapshot);
        for (ActiveEvent& e : _activeEvents)
            if (e.tickAt == key.first && e.eventId == key.second)
            {
                e.state              = snapshot.state;
                for (ObjectGuid const& g : e.spawnedCreatures)
                {
                    _eventBossSpawnIndex.erase(g.GetRawValue());
                    _eventBossTierMap.erase(g.GetRawValue());
                    spawnIndexChanged = true;
                }
                e.spawnedCreatures.clear();
                e.spawnedGameObjects.clear();
                break;
            }
        eventsChanged = true;
    }
    if (!toPrune.empty())
    {
        // Erase high-to-low so indices stay valid.
        std::sort(toPrune.begin(), toPrune.end(), std::greater<size_t>());
        for (size_t idx : toPrune)
            if (idx < _activeEvents.size())
                _activeEvents.erase(_activeEvents.begin()
                                    + static_cast<std::ptrdiff_t>(idx));
        eventsChanged = true;
    }

    if (spawnIndexChanged)
    {
        PublishEventBossSpawnSnap();
        PublishCombatHot();
    }
    if (eventsChanged)
        PublishEventsSnap();

    // Refresh the per-player stalker aura on every ACTIVE world boss so
    // the boss shows on each viewer's minimap. Cheap at 1Hz — the aura
    // only applies if the player doesn't already have one cast on the
    // boss (duration re-check is free).
    MarkWorldBossForPlayers();
}

namespace
{
    // Reserved quest ID range carrying Slice 6 worldmap POIs. See
    // migration rev_1777084800000000004.sql — IDs 90100-90120 are
    // one-per-zone dummy quests whose quest_poi rows point at each
    // boss's anchor. Kept in sync with the migration; if the range
    // grows, bump the max.
    constexpr uint32 TRACKER_QUEST_ID_MIN = 90100;
    constexpr uint32 TRACKER_QUEST_ID_MAX = 90120;

    bool IsTrackerQuestId(uint32 qid)
    {
        return qid >= TRACKER_QUEST_ID_MIN && qid <= TRACKER_QUEST_ID_MAX;
    }

    // Clean-revoke a tracker quest from a player's log. Mirrors the
    // CMSG_QUESTLOG_REMOVE_QUEST handler flow minus the achievement
    // credit + side-effects — our dummy quest has no items, timers,
    // or PVP flags, so the happy path is: clear the slot + erase
    // m_QuestStatus + push the update. Safe even if the quest isn't
    // actually in the log (early-out on slot lookup).
    void RevokeTrackerQuest(Player* p, uint32 questId)
    {
        if (!p) return;
        uint16 slot = p->FindQuestSlot(questId);
        if (slot >= MAX_QUEST_LOG_SIZE)
            return;
        p->RemoveActiveQuest(questId, /*update*/ true);
        p->SetQuestSlot(slot, 0);
    }
}

void TerrorZonesMgr::MarkWorldBossForPlayers()
{
    if (!_eventsEnabled)
        return;

    // World-thread-only — `_activeEvents` is the writer-side mutable
    // copy; we're already on the same thread that mutates it, so a
    // direct read is safe.
    struct BossRef {
        uint32 zoneId;
        uint32 mapId;
        ObjectGuid guid;
        uint32 trackerQuestId;
    };
    std::vector<BossRef> bosses;
    for (ActiveEvent const& e : _activeEvents)
    {
        if (e.state != EVENT_STATE_ACTIVE) continue;
        if (e.type != EVENT_WORLD_BOSS) continue;
        if (e.spawnedCreatures.empty()) continue;
        bosses.push_back({e.zoneId, e.mapId,
                          e.spawnedCreatures[0],
                          e.trackerQuestId});
    }

    // Build (zoneId → trackerQuestId) map of zones with a live
    // event. Used below for the revoke sweep — a player holding a
    // tracker quest whose zone isn't in this map should have it
    // removed (either they left the zone, or the event ended).
    std::unordered_map<uint32, uint32> activeZoneTracker;
    activeZoneTracker.reserve(bosses.size());
    for (BossRef const& b : bosses)
        if (b.trackerQuestId != 0)
            activeZoneTracker[b.zoneId] = b.trackerQuestId;

    WorldSessionMgr::SessionMap const& sessions =
        sWorldSessionMgr->GetAllSessions();

    // Pass 1: aura refresh + quest grant. Only for players in a
    // zone with a live world-boss event.
    for (BossRef const& b : bosses)
    {
        Map* map = sMapMgr->FindBaseNonInstanceMap(b.mapId);
        if (!map) continue;
        Creature* boss = map->GetCreature(b.guid);
        if (!boss || !boss->IsAlive() || !boss->IsInWorld())
            continue;

        Quest const* trackerQuest = (b.trackerQuestId != 0)
            ? sObjectMgr->GetQuestTemplate(b.trackerQuestId)
            : nullptr;

        for (auto const& kv : sessions)
        {
            WorldSession* session = kv.second;
            if (!session) continue;
            Player* p = session->GetPlayer();
            if (!p || !p->IsInWorld())
                continue;
            if (p->GetZoneId() != b.zoneId)
                continue;

            // Hunter's-Mark-style stalker aura → minimap dot for
            // this specific player.
            if (_eventBossTrackerSpellId != 0
                && !boss->HasAuraTypeWithCaster(
                        SPELL_AURA_MOD_STALKED, p->GetGUID()))
            {
                p->AddAura(_eventBossTrackerSpellId, boss);
            }

            // Worldmap / minimap POI → grant the tracker quest if
            // the player doesn't have it already AND has a free
            // quest-log slot. We deliberately bypass
            // `CanTakeQuest` prerequisite checks — the quest is a
            // hidden helper, not a real quest.
            if (trackerQuest
                && p->FindQuestSlot(b.trackerQuestId)
                       >= MAX_QUEST_LOG_SIZE
                && p->SatisfyQuestLog(/*msg*/ false))
            {
                p->AddQuest(trackerQuest, nullptr);
            }
        }
    }

    // Pass 2: revoke-sweep for every online player. A tracker
    // quest in the log without a matching active event in the
    // player's current zone gets removed. Handles event-end,
    // zone-leave, and logins-into-a-stale-quest in one loop.
    for (auto const& kv : sessions)
    {
        WorldSession* session = kv.second;
        if (!session) continue;
        Player* p = session->GetPlayer();
        if (!p || !p->IsInWorld())
            continue;

        uint32 playerZone = p->GetZoneId();
        auto it = activeZoneTracker.find(playerZone);
        uint32 keepQuestId = (it != activeZoneTracker.end())
                               ? it->second : 0;

        for (uint32 qid = TRACKER_QUEST_ID_MIN;
             qid <= TRACKER_QUEST_ID_MAX; ++qid)
        {
            if (qid == keepQuestId)
                continue;
            RevokeTrackerQuest(p, qid);
        }
    }
}

// -----------------------------------------------------------------------------
// Fire / end / spawn helpers
// -----------------------------------------------------------------------------

void TerrorZonesMgr::FireEvent(ActiveEvent& evt)
{
    // World-thread-only — `_eventBossDefs` and `_eventNodeSurgeDefs`
    // are write-once at LoadEventContent; safe to read directly.
    switch (evt.type)
    {
        case EVENT_WORLD_BOSS:
        {
            EventBossDef const* def = nullptr;
            for (EventBossDef const& d : _eventBossDefs)
                if (d.id == evt.definitionId)
                {
                    def = &d;
                    break;
                }
            if (def)
                SpawnWorldBoss(evt, *def);
            break;
        }
        case EVENT_RARE_NODE_SURGE:
        {
            EventNodeSurgeDef const* def = nullptr;
            for (EventNodeSurgeDef const& d : _eventNodeSurgeDefs)
                if (d.id == evt.definitionId)
                {
                    def = &d;
                    break;
                }
            if (def)
                SpawnNodeSurge(evt, *def);
            break;
        }
        default:
            break;
    }

    evt.state = EVENT_STATE_ACTIVE;
    PersistEventState(evt);

    // Zone-scoped announcement (§6.4).
    std::string line;
    {
        char buf[256];
        uint32 remainingSec =
            (evt.endsAt > evt.startsAt)
                ? static_cast<uint32>(evt.endsAt - evt.startsAt)
                : _eventCfg.durationSec;
        switch (evt.type)
        {
            case EVENT_WORLD_BOSS:
                std::snprintf(buf, sizeof(buf),
                    "|cffff8040A dreadful presence stirs — %s has appeared "
                    "near (%.0f, %.0f). (~%u minutes.)|r",
                    evt.displayName.c_str(),
                    evt.anchorX, evt.anchorY, remainingSec / 60);
                break;
            case EVENT_RARE_NODE_SURGE:
                std::snprintf(buf, sizeof(buf),
                    "|cffff8040A rich vein swells through %s at "
                    "(%.0f, %.0f) — gatherers, head out. (~%u minutes.)|r",
                    evt.displayName.c_str(),
                    evt.anchorX, evt.anchorY, remainingSec / 60);
                break;
            default:
                std::snprintf(buf, sizeof(buf),
                    "|cffff8040A %s event has begun. (~%u minutes.)|r",
                    EventTypeDisplayName(evt.type), remainingSec / 60);
                break;
        }
        line = buf;
    }
    uint32 zoneId = evt.zoneId;
    BroadcastZoneLineGated(zoneId, line, ANNOUNCE_EVENT_START);

    LOG_INFO("module",
             "mod-terror-zones: event fired type={} def={} zone={} "
             "spawns=[creatures={},objects={}] ends_at={}",
             EventTypeDisplayName(evt.type), evt.definitionId, evt.zoneId,
             static_cast<uint32>(evt.spawnedCreatures.size()),
             static_cast<uint32>(evt.spawnedGameObjects.size()),
             evt.endsAt);
}

void TerrorZonesMgr::EndEvent(ActiveEvent& evt)
{
    DespawnEventCreatures(evt);
    DespawnEventGameObjects(evt);

    evt.state = EVENT_STATE_EXPIRED;
    PersistEventState(evt);

    std::string line;
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "|cffff8040%s's presence fades — the rotation's event has ended.|r",
            evt.displayName.c_str());
        line = buf;
    }
    uint32 zoneId = evt.zoneId;
    BroadcastZoneLineGated(zoneId, line, ANNOUNCE_EVENT_END);

    LOG_INFO("module",
             "mod-terror-zones: event ended type={} def={} zone={}",
             EventTypeDisplayName(evt.type), evt.definitionId, evt.zoneId);
}

uint8 TerrorZonesMgr::ComputeEventBossApex(EventBossDef const& def) const
{
    // Server-wide apex across real-player sessions (bots excluded).
    // Mirrors ComputeTargetLevel's player walk, but is callable
    // regardless of rotation state. Event bosses always scale to the
    // apex so a GM force-fire in a non-empowered zone still produces
    // a credible fight.
    uint8 highest = 0;
    WorldSessionMgr::SessionMap const& sessions =
        sWorldSessionMgr->GetAllSessions();
    for (auto const& kv : sessions)
    {
        WorldSession* session = kv.second;
        if (!session || session->IsBot())
            continue;
        Player* p = session->GetPlayer();
        if (!p || !p->IsInWorld())
            continue;
        if (p->IsGameMaster())
            continue;
        uint8 lvl = p->GetLevel();
        if (lvl > highest)
            highest = lvl;
    }

    // `_pool` / `_poolIndex` are write-once at LoadPool.
    uint16 poolLevelMax = 0;
    auto it = _poolIndex.find(def.zoneId);
    if (it != _poolIndex.end())
        poolLevelMax = _pool[it->second].levelMax;

    uint8 apex = std::max<uint8>(
        {static_cast<uint8>(poolLevelMax), highest, def.levelMax});
    if (apex < def.levelMin)
        apex = def.levelMin;
    // CreatureBaseStats ships data up to level 83; anything above
    // that reads zero stats. Clamp defensively.
    if (apex > 83)
        apex = 83;
    return apex;
}

void TerrorZonesMgr::SpawnWorldBoss(ActiveEvent& evt,
                                    EventBossDef const& def)
{
    Map* map = sMapMgr->FindBaseNonInstanceMap(def.anchorMap);
    if (!map)
    {
        LOG_WARN("module",
                 "mod-terror-zones: event boss spawn — base map {} not loaded; "
                 "boss def {} deferred (will retry on restart-resume).",
                 def.anchorMap, def.id);
        return;
    }
    Position pos{ def.anchorX, def.anchorY, def.anchorZ, def.anchorO };
    // Summon for the event's actual remaining window rather than the
    // global DurationSec: the AlwaysSpawn boss runs the FULL rotation
    // (endsAt = tickAt + intervalSec), which is longer than DurationSec.
    // For normal events (endsAt = startsAt + DurationSec, fired at
    // ~startsAt) this resolves to ~DurationSec, so behavior is unchanged.
    uint64 nowSec = static_cast<uint64>(::time(nullptr));
    uint32 windowSec = (evt.endsAt > nowSec)
        ? static_cast<uint32>(evt.endsAt - nowSec)
        : _eventCfg.durationSec;
    uint32 durMs = windowSec * 1000;

    // Force the scaling override before SummonCreature. The hook
    // fires synchronously inside Create → SelectLevel on this same
    // thread, so the override is live for exactly the one
    // SelectLevel call we care about. Cleared in a guard below.
    uint8 apex = ComputeEventBossApex(def);

    // Resolve the source rotation slot's tier so HP scaling can
    // pin to it (decoupled from the boss's spawn zone). GM-forced
    // events use slotIndex 0xFFFF — fall back to TIER_5.
    Tier eventTier = TIER_5;
    if (evt.slotIndex < _rotation.slots.size())
        eventTier = _rotation.slots[evt.slotIndex].tier;
    if (eventTier == TIER_NONE)
        eventTier = TIER_5;

    _eventBossScaleOverride.store(apex, std::memory_order_relaxed);
    _eventBossSpawnPending.store(true, std::memory_order_relaxed);
    _eventBossTierOverride.store(static_cast<uint8>(eventTier),
                                  std::memory_order_relaxed);

    TempSummon* boss = map->SummonCreature(
        def.creatureTemplateId, pos, nullptr, durMs, nullptr);

    // Always clear the override flags immediately after the summon,
    // whether it succeeded or not. Subsequent summons in this call
    // (beacon, future markers) MUST NOT pick up either flag.
    _eventBossScaleOverride.store(0, std::memory_order_relaxed);
    _eventBossSpawnPending.store(false, std::memory_order_relaxed);
    _eventBossTierOverride.store(0, std::memory_order_relaxed);

    if (!boss)
    {
        LOG_WARN("module",
                 "mod-terror-zones: event boss SummonCreature failed — "
                 "template={} map={} pos=({:.1f},{:.1f},{:.1f})",
                 def.creatureTemplateId, def.anchorMap,
                 def.anchorX, def.anchorY, def.anchorZ);
        return;
    }

    if (_debug)
        LOG_INFO("module",
                 "mod-terror-zones: event boss spawned entry={} level={} "
                 "(apex={}) zone={} pos=({:.1f},{:.1f},{:.1f})",
                 def.creatureTemplateId,
                 static_cast<uint32>(boss->GetLevel()),
                 static_cast<uint32>(apex), evt.zoneId,
                 def.anchorX, def.anchorY, def.anchorZ);

    // Scale up so the boss reads as dramatic-sized. Runtime-only — we
    // don't mutate creature_template. Clamp guard applies in LoadConfig.
    if (_eventBossScaleMult > 0.0f && _eventBossScaleMult != 1.0f)
        boss->SetObjectScale(boss->GetObjectScale() * _eventBossScaleMult);
    ObjectGuid bossGuid = boss->GetGUID();
    evt.spawnedCreatures.push_back(bossGuid);
    // NOTE: `_eventBossSpawnIndex` commit + snapshot publish happens
    // in TickEvents / FireEventNow once FireEvent returns. We don't
    // touch the index here so callers stay in control of the
    // commit-then-publish ordering.

    // Beacon GO at the anchor so players can spot the event from
    // across the zone. Any gameobject entry works — default is a
    // vertical Energy Column (191763). Admins can swap via
    // TerrorZones.Events.WorldBoss.BeaconGameObjectId=0 to disable.
    if (_eventBossBeaconGoId != 0)
    {
        GameObject* beacon = map->SummonGameObject(
            _eventBossBeaconGoId, def.anchorX, def.anchorY, def.anchorZ,
            def.anchorO, 0.0f, 0.0f, 0.0f, 0.0f,
            windowSec, /*checkTransport*/ false);
        if (beacon)
            evt.spawnedGameObjects.push_back(beacon->GetGUID());
        else if (_debug)
            LOG_INFO("module",
                     "mod-terror-zones: event boss beacon spawn failed — "
                     "go_entry={} map={} (template missing?)",
                     _eventBossBeaconGoId, def.anchorMap);
    }
}

void TerrorZonesMgr::SpawnNodeSurge(ActiveEvent& evt,
                                    EventNodeSurgeDef const& def)
{
    Map* map = sMapMgr->FindBaseNonInstanceMap(def.anchorMap);
    if (!map)
    {
        LOG_WARN("module",
                 "mod-terror-zones: node surge spawn — base map {} not loaded; "
                 "def {} deferred.", def.anchorMap, def.id);
        return;
    }
    float radius = def.radius > 0.0f
                 ? def.radius : _eventNodeSurgeDefaultRadius;
    uint32 nodeCount = def.nodeCount > 0
                     ? def.nodeCount : _eventNodeSurgeDefaultCount;
    if (def.nodeIds.empty())
        return;

    // Seed uniquely per event so two node surges in the same tick have
    // different placements.
    uint64 seed = static_cast<uint64>(evt.tickAt)
                ^ (static_cast<uint64>(evt.eventId) << 32)
                ^ 0xA5A5A5A5A5A5A5A5ULL;
    StdRng rng(seed);

    // Cluster-center beacon so players can find the surge from distance.
    // Goes in first so it's at the exact anchor Z (not height-recovered).
    if (_eventNodeSurgeBeaconGoId != 0)
    {
        GameObject* beacon = map->SummonGameObject(
            _eventNodeSurgeBeaconGoId,
            def.anchorX, def.anchorY, def.anchorZ,
            0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            _eventCfg.durationSec, /*checkTransport*/ false);
        if (beacon)
            evt.spawnedGameObjects.push_back(beacon->GetGUID());
    }

    uint32 spawned = 0;
    for (uint32 i = 0; i < nodeCount; ++i)
    {
        uint32 goEntry =
            def.nodeIds[rng.NextUInt(
                static_cast<uint32>(def.nodeIds.size()))];
        float nx, ny;
        PickSubregionAnchor(def.anchorX, def.anchorY, radius,
                             rng, nx, ny);
        // Recover a valid Z via the map's height lookup from a point
        // above the anchor's Z. If that fails, fall back to the
        // anchor Z — worst case the node floats 2m off the ground.
        float probeZ = def.anchorZ + _eventNodeSurgeZIgnore;
        float nz = map->GetHeight(nx, ny, probeZ, true);
        // Map::GetHeight returns INVALID_HEIGHT (~-100000) when no
        // terrain data is loaded at (x,y). Fall back to the anchor Z
        // so worst case the node floats a few meters off the ground
        // rather than through the world.
        if (nz < -10000.0f)
            nz = def.anchorZ;

        // respawnTime=_eventCfg.durationSec marks the GO as temporary
        // (SpellId=1 per Map::SummonGameObject), and SpawnedByDefault
        // is set to false so it doesn't auto-respawn after gather.
        // We despawn anything still alive at EndEvent.
        GameObject* go = map->SummonGameObject(
            goEntry, nx, ny, nz, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            _eventCfg.durationSec, /*checkTransport*/ false);
        if (go)
        {
            evt.spawnedGameObjects.push_back(go->GetGUID());
            ++spawned;
        }
    }

    if (_debug)
        LOG_INFO("module",
                 "mod-terror-zones: node surge spawn def={} zone={} "
                 "anchor=({:.1f},{:.1f},{:.1f}) radius={:.1f} spawned={}/{}",
                 def.id, def.zoneId,
                 def.anchorX, def.anchorY, def.anchorZ, radius,
                 spawned, nodeCount);
}

void TerrorZonesMgr::DespawnEventCreatures(ActiveEvent& evt)
{
    // `_eventBossSpawnIndex` erase + snapshot publish happens in the
    // caller (TickEvents / EndActiveEventsInZone) once EndEvent
    // returns — stays in control of the commit-then-publish ordering.
    if (evt.spawnedCreatures.empty())
        return;
    Map* map = sMapMgr->FindBaseNonInstanceMap(evt.mapId);
    for (ObjectGuid const& guid : evt.spawnedCreatures)
    {
        if (!map)
            continue;
        Creature* c = map->GetCreature(guid);
        if (!c)
            continue;  // already auto-despawned by TempSummon timer
        c->DespawnOrUnsummon();
    }
    evt.spawnedCreatures.clear();
}

void TerrorZonesMgr::DespawnEventGameObjects(ActiveEvent& evt)
{
    if (evt.spawnedGameObjects.empty())
        return;
    Map* map = sMapMgr->FindBaseNonInstanceMap(evt.mapId);
    for (ObjectGuid const& guid : evt.spawnedGameObjects)
    {
        if (!map)
            continue;
        GameObject* go = map->GetGameObject(guid);
        if (!go)
            continue;  // already gathered / cleaned up
        go->Delete();
    }
    evt.spawnedGameObjects.clear();
}

void TerrorZonesMgr::BroadcastZoneLineGated(uint32 zoneId,
                                              std::string const& line,
                                              AnnounceCategory cat)
{
    if (zoneId == 0 || line.empty())
        return;
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
        if (!IsCategoryEnabledFor(p, cat))
            continue;
        ChatHandler(session).PSendSysMessage("{}", line);
    }
}

void TerrorZonesMgr::SendEventEndingCountdown(ActiveEvent const& evt,
                                                uint32 remainingSec)
{
    if (evt.zoneId == 0)
        return;
    char buf[256];
    if (remainingSec >= 60)
    {
        uint32 mins = (remainingSec + 30) / 60;
        std::snprintf(buf, sizeof(buf),
            "|cffff8040%s 准备撤退 — %u 分钟后事件结束。|r",
            evt.displayName.c_str(), mins, (mins == 1 ? "" : "s"));
    }
    else
    {
        std::snprintf(buf, sizeof(buf),
            "|cffff8040%s 准备撤退 — %u 秒后事件结束。|r",
            evt.displayName.c_str(), remainingSec);
    }
    BroadcastZoneLineGated(evt.zoneId, buf, ANNOUNCE_EVENT_ENDING);
}

void TerrorZonesMgr::BroadcastZoneLine(uint32 zoneId,
                                       std::string const& line)
{
    if (zoneId == 0 || line.empty())
        return;
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
        ChatHandler(session).PSendSysMessage("{}", line);
    }
}

void TerrorZonesMgr::PersistEventState(ActiveEvent const& evt)
{
    CharacterDatabase.Execute(
        "UPDATE terror_zones_events SET state = {} "
        "WHERE tick_at = {} AND slot_index = {} AND event_id = {}",
        static_cast<uint32>(evt.state),
        evt.tickAt, evt.slotIndex, evt.eventId);
}

void TerrorZonesMgr::PersistEventCountdownFired(ActiveEvent const& evt)
{
    CharacterDatabase.Execute(
        "UPDATE terror_zones_events SET countdown_fired = 1 "
        "WHERE tick_at = {} AND slot_index = {} AND event_id = {}",
        evt.tickAt, evt.slotIndex, evt.eventId);
}

void TerrorZonesMgr::PruneExpiredEventDbRows()
{
    if (_eventRetentionHours == 0)
        return;
    uint64 now = static_cast<uint64>(::time(nullptr));
    uint64 cutoff = now > static_cast<uint64>(_eventRetentionHours) * 3600
        ? now - static_cast<uint64>(_eventRetentionHours) * 3600
        : 0;
    if (cutoff == 0)
        return;
    CharacterDatabase.Execute(
        "DELETE FROM terror_zones_events WHERE ends_at < {}", cutoff);
}

// -----------------------------------------------------------------------------
// GM command entry points
// -----------------------------------------------------------------------------

uint32 TerrorZonesMgr::FireEventNow(Player* gm, EventType type)
{
    if (!gm || type == EVENT_NONE || !_eventsEnabled)
        return 0;

    // Event bosses are tied to the current TZ rotation. If GM is in
    // a rotation zone, use that zone. Otherwise pick the first
    // rotation slot's zone — keeps event bosses inside empowered
    // zones so class drops + tier scaling apply correctly.
    uint32 zoneId = gm->GetZoneId();
    bool gmZoneInRotation = false;
    for (ActiveSlot const& s : _rotation.slots)
    {
        if (s.zoneId == zoneId)
        {
            gmZoneInRotation = true;
            break;
        }
    }
    if (!gmZoneInRotation)
    {
        if (_rotation.slots.empty())
        {
            LOG_WARN("module",
                     "mod-terror-zones: .zones event fire — no active "
                     "rotation; can't tie event boss to a TZ zone.");
            return 0;
        }
        zoneId = _rotation.slots.front().zoneId;
        LOG_INFO("module",
                 "mod-terror-zones: .zones event fire — GM zone {} "
                 "not in rotation; redirecting event to rotation zone {}.",
                 gm->GetZoneId(), zoneId);
    }
    if (zoneId == 0)
        return 0;

    // Resolve a content def for the zone. GM forcing IGNORES the level
    // band — the band filter is for auto-scheduling (pick something
    // sensibly-scaled for the group level). When a GM types .zones
    // event fire, they want the curated definition for this zone
    // regardless of their character level. Zone match + enabled is the
    // only filter here.
    // Content defs are write-once at LoadEventContent.
    uint32 defId = 0, mapId = 0, trackerQuestId = 0;
    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    std::string name;
    if (type == EVENT_WORLD_BOSS)
    {
        for (EventBossDef const& d : _eventBossDefs)
            if (d.zoneId == zoneId)
            {
                defId = d.id; mapId = d.anchorMap;
                ax = d.anchorX; ay = d.anchorY; az = d.anchorZ;
                name = d.displayName.empty()
                    ? std::string("World Boss") : d.displayName;
                trackerQuestId = d.trackerQuestId;
                break;
            }
    }
    else if (type == EVENT_RARE_NODE_SURGE)
    {
        for (EventNodeSurgeDef const& d : _eventNodeSurgeDefs)
            if (d.zoneId == zoneId)
            {
                defId = d.id; mapId = d.anchorMap;
                ax = d.anchorX; ay = d.anchorY; az = d.anchorZ;
                name = d.displayName.empty()
                    ? std::string("a rare surge") : d.displayName;
                break;
            }
    }
    if (defId == 0)
    {
        LOG_WARN("module",
                 "mod-terror-zones: .zones event fire — no {} content "
                 "curated for zone {}.",
                 EventTypeDisplayName(type), zoneId);
        return 0;
    }

    uint64 now = static_cast<uint64>(::time(nullptr));
    ActiveEvent evt{};
    // Use current rotation's tickAt when one exists; otherwise the
    // wall-clock boundary. The tuple (tickAt, slotIndex, eventId) just
    // has to be unique — for GM-forced events, (now, 0, auto) is fine.
    evt.tickAt         = now;
    evt.slotIndex      = 0xFFFF;        // distinct band, GM-forced
    evt.eventId        = 0;
    evt.type           = type;
    evt.state          = EVENT_STATE_PENDING;
    evt.startsAt       = now;
    evt.endsAt         = now + _eventCfg.durationSec;
    evt.zoneId         = zoneId;
    evt.mapId          = mapId;
    evt.definitionId   = defId;
    evt.anchorX        = ax;
    evt.anchorY        = ay;
    evt.anchorZ        = az;
    evt.displayName    = std::move(name);
    evt.trackerQuestId = trackerQuestId;

    // Dedupe: if a GM already has an event at tickAt=now / slot=0xFFFF,
    // bump eventId.
    {
        uint32 maxId = 0;
        bool hasMax = false;
        for (ActiveEvent const& e : _activeEvents)
            if (e.tickAt == evt.tickAt && e.slotIndex == evt.slotIndex)
            {
                hasMax = true;
                if (e.eventId >= maxId) maxId = e.eventId + 1;
            }
        if (hasMax)
            evt.eventId = maxId;
        _activeEvents.push_back(evt);
    }
    PublishEventsSnap();

    CharacterDatabase.Execute(
        "INSERT INTO terror_zones_events (tick_at, slot_index, event_id, "
        "event_type, state, starts_at, ends_at, zone_id, map_id, "
        "definition_id, anchor_x, anchor_y, anchor_z, countdown_fired) "
        "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, 0) "
        "ON DUPLICATE KEY UPDATE state=VALUES(state), countdown_fired=0",
        evt.tickAt, evt.slotIndex, evt.eventId,
        static_cast<uint32>(evt.type), static_cast<uint32>(evt.state),
        evt.startsAt, evt.endsAt, evt.zoneId, evt.mapId, evt.definitionId,
        evt.anchorX, evt.anchorY, evt.anchorZ);

    // Fire immediately — it's a GM command, instant feedback matters.
    FireEvent(evt);
    bool spawnIndexChanged = false;
    for (ActiveEvent& e : _activeEvents)
        if (e.tickAt == evt.tickAt && e.slotIndex == evt.slotIndex
            && e.eventId == evt.eventId)
        {
            e.state              = evt.state;
            e.spawnedCreatures   = evt.spawnedCreatures;
            e.spawnedGameObjects = evt.spawnedGameObjects;
            // GM-forced events have slotIndex 0xFFFF — fall back to
            // T5 since slot lookup won't resolve. For non-GM-forced
            // events that hit this path, use the slot's tier.
            Tier slotTier = TIER_5;
            if (e.slotIndex < _rotation.slots.size())
                slotTier = _rotation.slots[e.slotIndex].tier;
            if (slotTier == TIER_NONE)
                slotTier = TIER_5;
            for (ObjectGuid const& g : evt.spawnedCreatures)
            {
                _eventBossSpawnIndex[g.GetRawValue()] =
                    {e.tickAt, e.eventId};
                _eventBossTierMap[g.GetRawValue()] = slotTier;
                spawnIndexChanged = true;
            }
            break;
        }
    PublishEventsSnap();
    if (spawnIndexChanged)
    {
        PublishEventBossSpawnSnap();
        PublishCombatHot();
    }

    return evt.eventId + 1;  // 1-based ID in command output
}

uint32 TerrorZonesMgr::EndActiveEventsInZone(uint32 zoneId)
{
    if (zoneId == 0)
        return 0;
    std::vector<std::pair<uint64, uint32>> toEnd;
    for (ActiveEvent const& e : _activeEvents)
    {
        if (e.zoneId != zoneId)
            continue;
        if (e.state == EVENT_STATE_EXPIRED)
            continue;
        toEnd.push_back({e.tickAt, e.eventId});
    }
    bool spawnIndexChanged = false;
    for (auto const& key : toEnd)
    {
        ActiveEvent snapshot;
        bool haveEvt = false;
        bool wasPending = false;
        for (ActiveEvent const& e : _activeEvents)
            if (e.tickAt == key.first && e.eventId == key.second
                && e.state != EVENT_STATE_EXPIRED)
            {
                snapshot = e;
                wasPending = (e.state == EVENT_STATE_PENDING);
                haveEvt = true;
                break;
            }
        if (!haveEvt)
            continue;
        if (wasPending)
        {
            // PENDING events never spawned — just mark expired.
            snapshot.state = EVENT_STATE_EXPIRED;
            PersistEventState(snapshot);
        }
        else
        {
            EndEvent(snapshot);   // despawns entities
        }
        for (ActiveEvent& e : _activeEvents)
            if (e.tickAt == key.first && e.eventId == key.second)
            {
                e.state = snapshot.state;
                for (ObjectGuid const& g : e.spawnedCreatures)
                {
                    _eventBossSpawnIndex.erase(g.GetRawValue());
                    _eventBossTierMap.erase(g.GetRawValue());
                    spawnIndexChanged = true;
                }
                e.spawnedCreatures.clear();
                e.spawnedGameObjects.clear();
                break;
            }
    }
    if (!toEnd.empty())
        PublishEventsSnap();
    if (spawnIndexChanged)
    {
        PublishEventBossSpawnSnap();
        PublishCombatHot();
    }
    return static_cast<uint32>(toEnd.size());
}

std::vector<ActiveEvent> TerrorZonesMgr::GetEventsSnapshot() const
{
    auto snap = std::atomic_load_explicit(&_eventsSnap,
                                           std::memory_order_acquire);
    return snap ? *snap : std::vector<ActiveEvent>{};
}

// -----------------------------------------------------------------------------
// Slice 8 — event-boss bonus loot pool
// -----------------------------------------------------------------------------

void TerrorZonesMgr::LoadEventBossLootPool()
{
    _eventBossLootPool.clear();

    QueryResult rows = WorldDatabase.Query(
        "SELECT id, level_min, level_max, guaranteed_blue_item_id, "
        "purple_item_id, purple_chance, gold_min_copper, gold_max_copper "
        "FROM terror_zones_event_bosses_loot_pool WHERE enabled = 1 "
        "ORDER BY level_min ASC");
    uint32 loaded = 0, skipped = 0;
    if (rows)
    {
        do
        {
            Field* f = rows->Fetch();
            LootPoolEntry e;
            e.id               = f[0].Get<uint32>();
            e.levelMin         = f[1].Get<uint8>();
            e.levelMax         = f[2].Get<uint8>();
            e.guaranteedBlueId = f[3].Get<uint32>();
            e.purpleItemId     = f[4].Get<uint32>();
            e.purpleChance     = f[5].Get<float>();
            e.goldMinCopper    = f[6].Get<uint32>();
            e.goldMaxCopper    = f[7].Get<uint32>();
            if (e.guaranteedBlueId != 0
                && !sObjectMgr->GetItemTemplate(e.guaranteedBlueId))
            {
                LOG_WARN("module",
                         "mod-terror-zones: loot pool id={} band=[{}-{}] "
                         "guaranteed_blue_item_id={} missing from "
                         "item_template, skipping row.",
                         e.id, static_cast<uint32>(e.levelMin),
                         static_cast<uint32>(e.levelMax),
                         e.guaranteedBlueId);
                ++skipped;
                continue;
            }
            if (e.purpleItemId != 0
                && !sObjectMgr->GetItemTemplate(e.purpleItemId))
            {
                LOG_WARN("module",
                         "mod-terror-zones: loot pool id={} band=[{}-{}] "
                         "purple_item_id={} missing; will skip purple roll.",
                         e.id, static_cast<uint32>(e.levelMin),
                         static_cast<uint32>(e.levelMax),
                         e.purpleItemId);
                e.purpleItemId = 0;
            }
            _eventBossLootPool.push_back(e);
            ++loaded;
        } while (rows->NextRow());
    }
    LOG_INFO("module",
             "mod-terror-zones: event-boss loot pool loaded — bands={} "
             "(skipped {}).",
             loaded, skipped);
}

TerrorZonesMgr::LootPoolEntry const* TerrorZonesMgr::FindEventBossLootBand(
    uint8 scaledLevel) const
{
    // `_eventBossLootPool` is write-once at LoadEventBossLootPool.
    // First-match wins; Pass-1 curation has no overlaps, so linear
    // scan is O(bands) ≤ ~10.
    for (LootPoolEntry const& e : _eventBossLootPool)
        if (scaledLevel >= e.levelMin && scaledLevel <= e.levelMax)
            return &e;
    return nullptr;
}

bool TerrorZonesMgr::TryEventBossDrop(Player const* player, Loot& loot)
{
    if (!player || !_enabled || !_eventsEnabled
        || !_eventBossLootPoolEnabled)
        return false;

    uint64 rawGuid = loot.sourceWorldObjectGUID.GetRawValue();
    if (rawGuid == 0)
        return false;

    // World-thread-only. Event-boss spawn index is the writer-side
    // mutable copy; loot path runs on the same world thread that
    // mutates it (TickEvents / FireEventNow / EndActiveEventsInZone).
    if (!_eventBossSpawnIndex.count(rawGuid))
        return false;  // not an event boss

    // Per-bundle dedup so TryEventBossDrop fires at most once per
    // Loot* bundle, matching the Slice 4 TryUniqueDrop shape. Without
    // this guard, OnBeforeDropAddItem would re-fire the injection per
    // template item the boss rolls — we'd get N copies of the
    // guaranteed blue. Uses its own set (distinct from
    // _uniqueRolledBundles) so unique drops + event-boss drops can
    // both fire on the same bundle.
    uint64 bundleKey = reinterpret_cast<uint64>(&loot);
    uint64 now = static_cast<uint64>(::time(nullptr));
    if (now - _eventBossLootRolledBundlesClearedAt > 30
        || _eventBossLootRolledBundles.size() > 10000)
    {
        _eventBossLootRolledBundles.clear();
        _eventBossLootRolledBundlesClearedAt = now;
    }
    if (!_eventBossLootRolledBundles.insert(bundleKey).second)
        return false;  // already injected for this bundle

    // Compute scaled level. Event bosses can outlive their rotation's
    // empowerment window (Slice 6 §2.9), so fall back to the player's
    // level when the zone isn't currently in rotation.
    uint8 highest = 0;
    {
        WorldSessionMgr::SessionMap const& sessions =
            sWorldSessionMgr->GetAllSessions();
        for (auto const& kv : sessions)
        {
            WorldSession* session = kv.second;
            if (!session || session->IsBot())
                continue;
            Player* p = session->GetPlayer();
            if (!p || !p->IsInWorld())
                continue;
            if (p->IsGameMaster())
                continue;
            uint8 lvl = p->GetLevel();
            if (lvl > highest)
                highest = lvl;
        }
    }
    uint32 playerZone = player->GetZoneId();
    bool inRotation = false;
    Tier zoneTier = TIER_NONE;
    for (ActiveSlot const& s : _rotation.slots)
    {
        if (s.zoneId == playerZone)
        {
            inRotation = true;
            zoneTier = s.tier;
            break;
        }
    }
    uint8 zoneMin = 0;
    auto poolIt = _poolIndex.find(playerZone);
    if (poolIt != _poolIndex.end() && poolIt->second < _pool.size())
        zoneMin = static_cast<uint8>(_pool[poolIt->second].levelMin);
    uint8 tierVal = (zoneTier >= TIER_1 && zoneTier <= TIER_5)
        ? static_cast<uint8>(zoneTier) : 0;
    uint8 scaled = ComputeTargetLevelPure(inRotation, highest,
                                           zoneMin, tierVal,
                                           _maxPlayerLevel);
    if (scaled == 0)
        scaled = player->GetLevel();

    LootPoolEntry const* band = FindEventBossLootBand(scaled);
    if (!band)
        return false;

    uint32 blueId       = band->guaranteedBlueId;
    uint32 purpleId     = band->purpleItemId;
    float  purpleChance = band->purpleChance * _eventBossLootPurpleMultiplier;
    if (purpleChance < 0.0f) purpleChance = 0.0f;
    if (purpleChance > 1.0f) purpleChance = 1.0f;

    bool injected = false;
    if (blueId != 0)
    {
        LootStoreItem blue(blueId, 0, 100.0f, false,
                            LOOT_MODE_DEFAULT, 0, 1, 1);
        loot.AddItem(blue);
        injected = true;
    }
    if (purpleId != 0 && roll_chance_f(purpleChance * 100.0f))
    {
        LootStoreItem purple(purpleId, 0, 100.0f, false,
                              LOOT_MODE_DEFAULT, 0, 1, 1);
        loot.AddItem(purple);
        injected = true;
    }
    // Gold target. We pre-seed loot.gold here so the gold pile is
    // visible when the loot window opens (creatures with native
    // mingold/maxgold = 0 wouldn't otherwise have a gold pile to
    // click). For creatures with non-zero native gold,
    // generateMoneyLoot (Unit.cpp:14084, fires AFTER FillLoot) will
    // overwrite our value — the OnPlayerBeforeLootMoney hook
    // restores via ApplyEventBossGoldUplift using
    // _eventBossGoldTargets.
    if (band->goldMaxCopper >= band->goldMinCopper
        && band->goldMaxCopper > 0)
    {
        uint32 goldAdd = urand(band->goldMinCopper, band->goldMaxCopper);
        if (goldAdd > 0)
        {
            uint64 bundleKey = reinterpret_cast<uint64>(&loot);
            _eventBossGoldTargets[bundleKey] = goldAdd;
            loot.gold = std::max(loot.gold, goldAdd);
            injected = true;
        }
    }

    if (injected && _debug)
        LOG_INFO("module",
                 "mod-terror-zones: event-boss loot injected — player_guid={} "
                 "scaled={} blue={} purple={} (chance={:.2f}) "
                 "[bundle readback: items={} gold={} unlooted={}]",
                 player->GetGUID().GetCounter(),
                 static_cast<uint32>(scaled),
                 blueId, purpleId, purpleChance,
                 static_cast<uint32>(loot.items.size()),
                 loot.gold,
                 static_cast<uint32>(loot.unlootedCount));

    // Slice 9 Pass 1 — class-targeted drop, additive on top of the
    // band-pool injection above. Has its own dedup, T1 skip, drop
    // chance, and cell-populated guard.
    TryClassDrop(player, loot);

    return injected;
}

void TerrorZonesMgr::ApplyEventBossGoldUplift(Loot& loot,
                                                Player const* player)
{
    if (!player || !_enabled || !_eventsEnabled
        || !_eventBossLootPoolEnabled)
        return;

    uint64 rawGuid = loot.sourceWorldObjectGUID.GetRawValue();
    if (rawGuid == 0)
        return;
    if (!_eventBossSpawnIndex.count(rawGuid))
        return;

    // Restore the band-pool gold target captured during
    // TryEventBossDrop. generateMoneyLoot may have clobbered
    // loot.gold to the creature's native (often tiny) value.
    // max() is idempotent — duplicate clicks are no-ops.
    uint64 bundleKey = reinterpret_cast<uint64>(&loot);
    auto it = _eventBossGoldTargets.find(bundleKey);
    if (it == _eventBossGoldTargets.end())
        return;
    uint32 target = it->second;
    uint32 prevGold = loot.gold;
    if (loot.gold < target)
        loot.gold = target;

    if (_debug)
        LOG_INFO("module",
                 "mod-terror-zones: event-boss gold uplift — "
                 "player_guid={} prev={} target={} final={}",
                 player->GetGUID().GetCounter(),
                 prevGold, target, loot.gold);
}

} // namespace mod_terror_zones
