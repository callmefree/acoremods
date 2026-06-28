// Slice 2 — combat scaling Mgr methods. The pure helpers
// (ComputeTargetLevelPure, ApplyScaling) live in TerrorZonesMath.cpp;
// this file implements the live wrapper that reads session / pool
// state and drives the OnBeforeCreatureSelectLevel hook.
//
// Slice 8 — post-SelectLevel HP mult + outgoing damage mult live
// here too (OnAfterCreatureSelectLevel, OnUnitDealDamage). Same
// eligibility predicate as Slice 2 level scaling.

#include "TerrorZonesMgr.h"

#include "Creature.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "TemporarySummon.h"
#include "Unit.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <limits>

namespace mod_terror_zones
{

uint8 TerrorZonesMgr::ComputeTargetLevel(uint32 zoneId) const
{
    if (!_enabled || !_scalingEnabled || zoneId == 0)
        return 0;

    auto rot = std::atomic_load_explicit(&_rotationSnap,
                                          std::memory_order_acquire);
    bool inRotation = false;
    Tier tier = TIER_NONE;
    if (rot)
    {
        for (ActiveSlot const& s : rot->slots)
        {
            if (s.zoneId == zoneId)
            {
                inRotation = true;
                tier = s.tier;
                break;
            }
        }
    }
    if (!inRotation)
        return 0;

    // Zone's natural level range from the TZ pool — the floor for
    // mob scaling so a zone never reads below its natural minimum.
    uint8 zoneMin = 0;
    auto poolIt = _poolIndex.find(zoneId);
    if (poolIt != _poolIndex.end() && poolIt->second < _pool.size())
        zoneMin = static_cast<uint8>(_pool[poolIt->second].levelMin);
    uint8 tierVal = (tier >= TIER_1 && tier <= TIER_5)
        ? static_cast<uint8>(tier) : 0;

    // Server-wide apex: highest level across all real-player sessions
    // (bots excluded). Zone presence intentionally ignored — on a 2-5
    // player server the apex player's level is what the rotation should
    // cater to regardless of where they physically are right now. This
    // also sidesteps the zone-id propagation race at creature spawn
    // time (player's session zone updates lag behind grid loading,
    // which would otherwise leave fresh spawns at the pool floor).
    //
    // IsInWorld() check intentionally absent: the AC teleport flow is
    // RemoveFromWorld → load destination map (creatures spawn here,
    // hitting OnBeforeCreatureSelectLevel) → AddToWorld. Same race on
    // login: SetPlayer runs before AddToWorld, and cell loading can
    // fire OnBeforeCreatureSelectLevel during the window in between.
    // Gating on IsInWorld() rejected the teleporting/logging-in
    // player's level, returned highest=0, and left fresh spawns at
    // their native level even though OnAfterCreatureSelectLevel was
    // happily applying the tier HP mult right after — producing
    // "level 11 boar with 33k HP" in the empowered zone. WorldSession
    // ::SetPlayer(nullptr) at logout (WorldSession.cpp:807) fires
    // before the Player is destroyed, so a `!p` check is enough to
    // reject genuinely-gone sessions; mid-cleanup sessions briefly
    // counted here are harmless (their level is still real and the
    // window is microseconds).
    uint8 highest = 0;
    WorldSessionMgr::SessionMap const& sessions =
        sWorldSessionMgr->GetAllSessions();
    for (auto const& kv : sessions)
    {
        WorldSession* session = kv.second;
        if (!session || session->IsBot())
            continue;
        Player* p = session->GetPlayer();
        if (!p)
            continue;
        uint8 lvl = p->GetLevel();
        if (lvl > highest)
            highest = lvl;
    }

    return ComputeTargetLevelPure(inRotation, highest,
                                   zoneMin, tierVal,
                                   _maxPlayerLevel);
}

bool TerrorZonesMgr::IsScalingEligible(Creature const* creature) const
{
    if (!creature)
        return false;
    if (creature->IsPet())
        return false;
    if (creature->IsSummon())
    {
        // Player-owned summons (totems, guardians, shadowfiends, etc.)
        // keep their native level. Script-owned TempSummons with no
        // summoner — e.g. our Slice 6 event world bosses — should
        // scale like normal empowered-zone mobs (per plan §5.1).
        if (TempSummon const* ts = creature->ToTempSummon())
        {
            if (ts->GetSummonerGUID() != ObjectGuid::Empty)
                return false;
            // else: ownerless TempSummon — allow scaling.
        }
        else
            return false;
    }
    if (creature->IsCritter())
        return false;
    if (creature->IsTrigger())
        return false;
    if (_scalingSkipWorldBosses && creature->isWorldBoss())
        return false;
    if (_scalingSkipFriendly)
    {
        // Skip only creatures whose faction is explicitly friendly to a
        // player team (vendors, guards, quest givers). "Not hostile" is
        // the wrong predicate: it incorrectly excludes neutral-aggressive
        // wildlife (boars, basilisks, wolves with faction EnemyGroup=0)
        // which have no faction-friendly bit and no faction-hostile bit —
        // they aggro via proximity, not faction.
        FactionTemplateEntry const* ft = sFactionTemplateStore.LookupEntry(
            creature->GetFaction());
        if (ft)
        {
            uint32 playerFriendlyBits = FACTION_MASK_PLAYER
                                      | FACTION_MASK_ALLIANCE
                                      | FACTION_MASK_HORDE;
            if ((ft->friendlyMask & playerFriendlyBits) != 0)
                return false;
        }
    }
    if (_scalingNeverEntries.count(creature->GetEntry()))
        return false;
    return true;
}

void TerrorZonesMgr::OnBeforeCreatureSelectLevel(Creature const* creature,
                                                 uint8& level)
{
    if (!_enabled || !_scalingEnabled || !creature)
        return;

    // Slice 6 — event-boss forced scale. SpawnWorldBoss sets the
    // override right before `map->SummonCreature` so the next
    // SelectLevel call (for THIS creature, same thread, synchronous)
    // lands here with the forced apex level. Bypasses rotation-state
    // + `SkipWorldBosses` eligibility because event bosses should
    // always scale, even in zones that aren't currently empowered
    // (GM force-fire) and even when the creature_template carries
    // CREATURE_TYPE_FLAG_BOSS_MOB (iconic elites often do).
    if (uint8 override = _eventBossScaleOverride.load(
            std::memory_order_relaxed))
    {
        uint8 outLevel = ApplyScaling(level, override);
        if (_debug && outLevel != level)
            LOG_INFO("module",
                     "mod-terror-zones: event-boss forced scale "
                     "entry={} from={} to={}",
                     creature->GetEntry(),
                     static_cast<uint32>(level),
                     static_cast<uint32>(outLevel));
        level = outLevel;
        return;
    }

    if (!IsScalingEligible(creature))
        return;

    uint32 zoneId = creature->GetZoneId();
    uint8 target = ComputeTargetLevel(zoneId);
    uint8 outLevel = ApplyScaling(level, target);
    if (outLevel == level)
        return;

    if (_debug)
        LOG_INFO("module",
                 "mod-terror-zones: scaled creature entry={} guid={} zone={} "
                 "from={} to={}",
                 creature->GetEntry(),
                 creature->GetGUID().GetCounter(),
                 zoneId, static_cast<uint32>(level),
                 static_cast<uint32>(outLevel));

    level = outLevel;
}

// Slice 8 — post-SelectLevel HP mult. Fires from AllCreatureScript::
// OnCreatureSelectLevel for every creature whose level was just
// computed. Reads a lock-free atomic snapshot of the rotation +
// event-boss GUID set (published by RunRotation / the event
// lifecycle), composes the combat HP mult, multiplies in place.
// No-op when the zone isn't empowered or the creature isn't
// eligible — edge-off tick walks land each creature back at native
// HP through this same path (SelectLevel re-runs, snapshot doesn't
// contain the zone, mult stays 1.0).
void TerrorZonesMgr::OnAfterCreatureSelectLevel(Creature* creature)
{
    if (!_enabled || !_combatEnabled || !creature)
        return;

    // Lock-free hot-path read. Publisher side has already built the
    // immutable snapshot; we just grab the shared_ptr.
    std::shared_ptr<CombatHotState const> hot =
        std::atomic_load_explicit(&_combatHot,
                                   std::memory_order_acquire);
    if (!hot || hot->slots.empty())
        return;

    uint32 zoneId = creature->GetZoneId();

    // Event-boss flag check FIRST — event bosses always get the
    // boss-tier scaling regardless of rotation/zone state. They
    // fire from event anchor locations decoupled from rotation
    // slots, so the boss's current zone is often NOT in any active
    // rotation slot. Without this bypass, event bosses spawned in
    // non-rotation zones get zero TZ scaling and read as native
    // level-80 mobs (~5-10k HP), defeating the whole "boss feel"
    // intent. The `_eventBossSpawnPending` flag covers the spawn
    // race where this hook fires DURING SummonCreature before the
    // GUID has been published into `eventBossGuids`.
    uint64 rawGuid = creature->GetGUID().GetRawValue();
    bool isEventBoss = hot->eventBossGuids.count(rawGuid) > 0
                    || _eventBossSpawnPending.load(
                            std::memory_order_relaxed);

    Tier tier = TIER_NONE;
    bool zoneMatch = false;
    for (CombatHotState::SlotView const& sv : hot->slots)
    {
        if (sv.zoneId == zoneId)
        {
            tier = sv.tier;
            zoneMatch = true;
            break;
        }
    }
    // For event bosses, prefer the source rotation slot's tier
    // (captured at spawn time) over the boss's current zone tier.
    // The slot tier is the design-correct answer regardless of
    // where the boss happens to be standing.
    if (isEventBoss)
    {
        uint8 ovr = _eventBossTierOverride.load(
            std::memory_order_relaxed);
        if (ovr >= TIER_1 && ovr <= TIER_5)
        {
            tier = static_cast<Tier>(ovr);
        }
        else
        {
            auto it = hot->eventBossTiers.find(rawGuid);
            if (it != hot->eventBossTiers.end())
                tier = it->second;
        }
    }

    if (!zoneMatch && tier == TIER_NONE)
        return;  // not an event boss + zone not empowered → bail
    if (!isEventBoss && !IsScalingEligible(creature))
        return;

    // Slice 8b — deterministic per-spawn elite-promotion. T1/T2 default
    // to threshold 0 → never promotes. Skipped for event bosses (their
    // own uplift is already the "this is bossy" signal — stacking the
    // elite mult on top would inflate HP into 18-32x territory at T5,
    // which the event-boss balance doesn't account for).
    bool isPromoted = false;
    if (!isEventBoss && tier != TIER_NONE && tier <= TIER_MAX)
        isPromoted = IsPromotedSpawn(rawGuid, hot->tickAt,
                                      _eliteDensityPerMille[tier]);

    float mult = ComputeCombatHpMult(_combatHpMult,
                                      tier,
                                      _tierHpBonus,
                                      isEventBoss,
                                      _eventBossHpMultUplift,
                                      isPromoted,
                                      _eliteHpMultUplift);
    if (mult <= 1.0f)
        return;  // nothing to do — floor reached

    uint32 baseHp = creature->GetMaxHealth();
    if (baseHp == 0)
        return;  // creature skipped stat computation (trigger, etc.)

    uint64 scaled = static_cast<uint64>(baseHp) * static_cast<uint64>(
        mult * 1000.0f) / 1000ULL;
    if (scaled > std::numeric_limits<uint32>::max())
        scaled = std::numeric_limits<uint32>::max();
    uint32 newHp = static_cast<uint32>(scaled);
    if (newHp < baseHp)  // degenerate — never shrink native HP
        newHp = baseHp;

    // AC's `Creature::SelectLevel` registers the PRE-mult health as
    // the BASE_VALUE of `UNIT_MOD_HEALTH` (Creature.cpp:1512). Any
    // subsequent `UpdateMaxHealth` (triggered by stat-mod refresh,
    // aura apply, gear equip, etc.) recomputes maxHP from that base
    // — silently undoing our SetMaxHealth here. Fix: also write our
    // scaled value into the BASE_VALUE stat slot so recomputes
    // produce the same number we set.
    creature->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE,
                                   static_cast<float>(newHp));

    creature->SetCreateHealth(newHp);
    creature->SetMaxHealth(newHp);
    creature->SetHealth(newHp);

    // Readback diagnostic — confirms the SetMaxHealth/SetHealth
    // calls actually took. If readback diverges from `newHp` then
    // something later in the spawn pipeline (TempSummon::Initialize,
    // a re-running SelectLevel, etc.) is overwriting our scaling.
    uint32 actualMaxHp = creature->GetMaxHealth();
    uint32 actualHp = creature->GetHealth();

    if (_debug)
        LOG_INFO("module",
                 "mod-terror-zones: combat HP mult entry={} guid={} zone={} "
                 "tier={} event_boss={} promoted={} mult=x{:.2f} from={} to={} "
                 "readback_max={} readback_cur={}{}",
                 creature->GetEntry(),
                 creature->GetGUID().GetCounter(),
                 zoneId,
                 static_cast<uint32>(tier),
                 isEventBoss,
                 isPromoted,
                 mult, baseHp, newHp,
                 actualMaxHp, actualHp,
                 (actualMaxHp != newHp ? " [DIVERGED]" : ""));
}

// Slice 8 — outgoing-damage mult. Fires from UnitScript::OnDamage for
// every damage dispatch (melee + spell + DoT) — hundreds of calls
// per second during combat. MUST be fully lock-free; reads the
// same atomic snapshot the HP hook uses.
void TerrorZonesMgr::OnUnitDealDamage(Unit* attacker, Unit* /*victim*/,
                                       uint32& damage)
{
    if (!_enabled || !_combatEnabled || !attacker || damage == 0)
        return;

    Creature const* c = attacker->ToCreature();
    if (!c)
        return;

    std::shared_ptr<CombatHotState const> hot =
        std::atomic_load_explicit(&_combatHot,
                                   std::memory_order_acquire);
    if (!hot || hot->slots.empty())
        return;

    uint32 zoneId = c->GetZoneId();
    uint64 rawGuid = c->GetGUID().GetRawValue();
    bool isEventBoss = hot->eventBossGuids.count(rawGuid) > 0;

    Tier tier = TIER_NONE;
    bool zoneMatch = false;
    for (CombatHotState::SlotView const& sv : hot->slots)
    {
        if (sv.zoneId == zoneId)
        {
            tier = sv.tier;
            zoneMatch = true;
            break;
        }
    }
    // Event-boss damage: prefer source slot tier from snapshot
    // (mirrors OnAfterCreatureSelectLevel's policy).
    if (isEventBoss)
    {
        auto it = hot->eventBossTiers.find(rawGuid);
        if (it != hot->eventBossTiers.end())
            tier = it->second;
    }

    if (!zoneMatch && tier == TIER_NONE)
        return;
    if (!isEventBoss && !IsScalingEligible(c))
        return;

    // Slice 8b — same promotion seed as the HP path so a creature
    // promoted in OnAfterCreatureSelectLevel is also promoted here.
    bool isPromoted = false;
    if (!isEventBoss && tier != TIER_NONE && tier <= TIER_MAX)
        isPromoted = IsPromotedSpawn(rawGuid, hot->tickAt,
                                      _eliteDensityPerMille[tier]);

    float mult = ComputeCombatDamageMult(_combatDamageMult,
                                          tier,
                                          _tierDamageBonus,
                                          isEventBoss,
                                          _eventBossDamageMultUplift,
                                          isPromoted,
                                          _eliteDamageMultUplift);
    if (mult <= 1.0f)
        return;

    uint64 scaled = static_cast<uint64>(damage) * static_cast<uint64>(
        mult * 1000.0f) / 1000ULL;
    if (scaled > std::numeric_limits<uint32>::max())
        scaled = std::numeric_limits<uint32>::max();
    damage = static_cast<uint32>(scaled);
}

void TerrorZonesMgr::WalkZoneRescale(uint32 zoneId, bool edgeOn,
                                      bool force)
{
    if (!_enabled || !_scalingEnabled)
        return;
    // `_scalingRescaleOnTick` gates the auto-tick edge rescale; GM
    // commands like `.zones settier` pass force=true to bypass.
    if (!force && !_scalingRescaleOnTick)
        return;

    uint32 walked = 0;
    uint32 scaled = 0;

    sMapMgr->DoForAllMaps([&](Map* map)
    {
        if (!map)
            return;
        // Only continents / non-instanced open-world maps carry the zones
        // we rotate over. Instance maps can't be empowered per spec §11.3.
        if (map->IsDungeon() || map->IsBattlegroundOrArena())
            return;

        auto& store = map->GetCreatureBySpawnIdStore();
        for (auto const& kv : store)
        {
            Creature* c = kv.second;
            if (!c || !c->IsInWorld())
                continue;
            if (c->GetZoneId() != zoneId)
                continue;
            ++walked;

            if (edgeOn && !IsScalingEligible(c))
                continue;
            // edgeOn=false (tick-off): rescale everything in the zone that
            // was once eligible, restoring baseline. Re-running SelectLevel
            // with our hook now observing zone-is-not-empowered produces
            // the baseline roll.

            c->SelectLevel(true);
            ++scaled;
        }
    });

    LOG_INFO("module",
             "mod-terror-zones: tick rescale zone={} edge={} creatures_walked={} scaled={}",
             zoneId, edgeOn ? "on" : "off", walked, scaled);
}

} // namespace mod_terror_zones
