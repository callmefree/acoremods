#include "MountProgressionMgr.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "World.h"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace mod_mount_progression
{

namespace
{
    MountRarity ParseRarity(std::string const& s)
    {
        if (s == "uncommon")  return MountRarity::Uncommon;
        if (s == "rare")      return MountRarity::Rare;
        if (s == "epic")      return MountRarity::Epic;
        if (s == "legendary") return MountRarity::Legendary;
        return MountRarity::Common;
    }

    MountType ParseType(std::string const& s)
    {
        if (s == "predator")   return MountType::Predator;
        if (s == "agility")    return MountType::Agility;
        if (s == "mechanical") return MountType::Mechanical;
        if (s == "arcane")     return MountType::Arcane;
        return MountType::Stamina;
    }
}

char const* RarityName(MountRarity r)
{
    switch (r)
    {
        case MountRarity::Common:    return "common";
        case MountRarity::Uncommon:  return "uncommon";
        case MountRarity::Rare:      return "rare";
        case MountRarity::Epic:      return "epic";
        case MountRarity::Legendary: return "legendary";
        default:                     return "unknown";
    }
}

char const* TypeName(MountType t)
{
    switch (t)
    {
        case MountType::Stamina:    return "stamina";
        case MountType::Predator:   return "predator";
        case MountType::Agility:    return "agility";
        case MountType::Mechanical: return "mechanical";
        case MountType::Arcane:     return "arcane";
        default:                    return "unknown";
    }
}

char const* XPSourceName(MountType t)
{
    switch (t)
    {
        case MountType::Stamina:    return "ride distance";
        case MountType::Predator:   return "kill creatures";
        case MountType::Agility:    return "explore new subzones";
        case MountType::Mechanical: return "gather and craft";
        case MountType::Arcane:     return "cast spells";
        default:                    return "unknown";
    }
}

MountProgressionMgr& MountProgressionMgr::Instance()
{
    static MountProgressionMgr inst;
    return inst;
}

void MountProgressionMgr::LoadConfig()
{
    _enabled   = sConfigMgr->GetOption<bool>("MountProgression.Enable", true);
    _debug     = sConfigMgr->GetOption<bool>("MountProgression.Debug", false);
    _maxLevel  = static_cast<uint16>(
        sConfigMgr->GetOption<uint32>("MountProgression.MaxLevel", 60));

    _xpBase[static_cast<size_t>(MountRarity::Common)] =
        sConfigMgr->GetOption<uint32>("MountProgression.XPBase.Common", 10);
    _xpBase[static_cast<size_t>(MountRarity::Uncommon)] =
        sConfigMgr->GetOption<uint32>("MountProgression.XPBase.Uncommon", 40);
    _xpBase[static_cast<size_t>(MountRarity::Rare)] =
        sConfigMgr->GetOption<uint32>("MountProgression.XPBase.Rare", 120);
    _xpBase[static_cast<size_t>(MountRarity::Epic)] =
        sConfigMgr->GetOption<uint32>("MountProgression.XPBase.Epic", 400);
    _xpBase[static_cast<size_t>(MountRarity::Legendary)] =
        sConfigMgr->GetOption<uint32>("MountProgression.XPBase.Legendary", 4000);

    _killXpNormal = sConfigMgr->GetOption<uint32>("MountProgression.KillXP.Normal", 10);
    _killXpElite  = sConfigMgr->GetOption<uint32>("MountProgression.KillXP.Elite", 40);
    _killXpBoss   = sConfigMgr->GetOption<uint32>("MountProgression.KillXP.Boss", 200);
    _killMinLevelDelta =
        sConfigMgr->GetOption<int32>("MountProgression.KillXP.MinLevelDelta", -10);

    _yardsPerXP = sConfigMgr->GetOption<uint32>(
        "MountProgression.DistanceXP.YardsPerXP", 10);
    _maxYardsPerTick = sConfigMgr->GetOption<uint32>(
        "MountProgression.DistanceXP.MaxYardsPerTick", 90);
    _castXPPerCast = sConfigMgr->GetOption<uint32>(
        "MountProgression.CastXP.PerCast", 1);
    _gatherXPPerLoot = sConfigMgr->GetOption<uint32>(
        "MountProgression.GatherXP.PerLoot", 5);
    _craftXPPerCraft = sConfigMgr->GetOption<uint32>(
        "MountProgression.CraftXP.PerCraft", 10);
    _areaXPPerTransition = sConfigMgr->GetOption<uint32>(
        "MountProgression.AreaXP.PerTransition", 25);

    _buffMagnitudeMode = static_cast<uint8>(sConfigMgr->GetOption<uint32>(
        "MountProgression.BuffMagnitudeMode", 1));
    _buffLevelCurve = static_cast<uint8>(sConfigMgr->GetOption<uint32>(
        "MountProgression.BuffLevelCurve", 1));
    _buffCeiling[static_cast<size_t>(MountRarity::Common)] =
        sConfigMgr->GetOption<uint32>("MountProgression.BuffCeiling.Common", 20);
    _buffCeiling[static_cast<size_t>(MountRarity::Uncommon)] =
        sConfigMgr->GetOption<uint32>("MountProgression.BuffCeiling.Uncommon", 60);
    _buffCeiling[static_cast<size_t>(MountRarity::Rare)] =
        sConfigMgr->GetOption<uint32>("MountProgression.BuffCeiling.Rare", 120);
    _buffCeiling[static_cast<size_t>(MountRarity::Epic)] =
        sConfigMgr->GetOption<uint32>("MountProgression.BuffCeiling.Epic", 200);
    _buffCeiling[static_cast<size_t>(MountRarity::Legendary)] =
        sConfigMgr->GetOption<uint32>("MountProgression.BuffCeiling.Legendary", 400);
    _offlineGraceSeconds = sConfigMgr->GetOption<uint32>(
        "MountProgression.OfflineGraceSeconds", 1800);
    _announceOnCast = sConfigMgr->GetOption<bool>(
        "MountProgression.AnnounceActiveOnCast", true);
    _carrierSpell[static_cast<size_t>(MountType::Stamina)] =
        sConfigMgr->GetOption<uint32>("MountProgression.CarrierSpell.Stamina", 80000);
    _carrierSpell[static_cast<size_t>(MountType::Predator)] =
        sConfigMgr->GetOption<uint32>("MountProgression.CarrierSpell.Predator", 80001);
    _carrierSpell[static_cast<size_t>(MountType::Agility)] =
        sConfigMgr->GetOption<uint32>("MountProgression.CarrierSpell.Agility", 80002);
    _carrierSpell[static_cast<size_t>(MountType::Mechanical)] =
        sConfigMgr->GetOption<uint32>("MountProgression.CarrierSpell.Mechanical", 80003);
    _carrierSpell[static_cast<size_t>(MountType::Arcane)] =
        sConfigMgr->GetOption<uint32>("MountProgression.CarrierSpell.Arcane", 80004);

    _iconDonor[static_cast<size_t>(MountType::Stamina)] =
        sConfigMgr->GetOption<uint32>("MountProgression.IconDonor.Stamina", 8099);
    _iconDonor[static_cast<size_t>(MountType::Predator)] =
        sConfigMgr->GetOption<uint32>("MountProgression.IconDonor.Predator", 30848);
    _iconDonor[static_cast<size_t>(MountType::Agility)] =
        sConfigMgr->GetOption<uint32>("MountProgression.IconDonor.Agility", 8115);
    _iconDonor[static_cast<size_t>(MountType::Mechanical)] =
        sConfigMgr->GetOption<uint32>("MountProgression.IconDonor.Mechanical", 77);
    _iconDonor[static_cast<size_t>(MountType::Arcane)] =
        sConfigMgr->GetOption<uint32>("MountProgression.IconDonor.Arcane", 22418);

    LOG_INFO("module",
             "mod-mount-progression: enabled={}, maxLevel={}, xpBase=[{},{},{},{},{}], "
             "killXp=[{}/{}/{}], dist(y/xp={} cap={}), cast={}, gather={}, craft={}, area={}, "
             "debug={}",
             _enabled, _maxLevel,
             _xpBase[0], _xpBase[1], _xpBase[2], _xpBase[3], _xpBase[4],
             _killXpNormal, _killXpElite, _killXpBoss,
             _yardsPerXP, _maxYardsPerTick,
             _castXPPerCast, _gatherXPPerLoot, _craftXPPerCraft, _areaXPPerTransition,
             _debug);
    LOG_INFO("module",
             "mod-mount-progression: buff mode={} curve={} ceil=[{},{},{},{},{}] "
             "grace={}s announce={} carriers=[{},{},{},{},{}] donors=[{},{},{},{},{}]",
             _buffMagnitudeMode, _buffLevelCurve,
             _buffCeiling[0], _buffCeiling[1], _buffCeiling[2],
             _buffCeiling[3], _buffCeiling[4],
             _offlineGraceSeconds, _announceOnCast,
             _carrierSpell[0], _carrierSpell[1], _carrierSpell[2],
             _carrierSpell[3], _carrierSpell[4],
             _iconDonor[0], _iconDonor[1], _iconDonor[2],
             _iconDonor[3], _iconDonor[4]);
}

uint32 MountProgressionMgr::GetIconDonor(uint32 realCarrierSpellId) const
{
    for (size_t i = 0; i < static_cast<size_t>(MountType::MAX); ++i)
        if (_carrierSpell[i] == realCarrierSpellId)
            return _iconDonor[i];
    return 0;
}

void MountProgressionMgr::LoadCatalog()
{
    _catalog.clear();

    QueryResult result = WorldDatabase.Query(
        "SELECT spell_id, display_id, display_name, rarity, type "
        "FROM mount_progression_catalog");

    if (!result)
    {
        LOG_WARN("module",
                 "mod-mount-progression: mount_progression_catalog is empty or missing. "
                 "Slice 1 migrations must be applied before this module is useful.");
        return;
    }

    do
    {
        Field* f = result->Fetch();
        CatalogEntry entry;
        entry.spellId     = f[0].Get<uint32>();
        entry.displayId   = f[1].Get<uint32>();
        entry.displayName = f[2].Get<std::string>();
        entry.rarity      = ParseRarity(f[3].Get<std::string>());
        entry.type        = ParseType(f[4].Get<std::string>());
        _catalog.emplace(entry.spellId, std::move(entry));
    } while (result->NextRow());

    LOG_INFO("module", "mod-mount-progression: loaded {} catalog entries.",
             _catalog.size());
}

CatalogEntry const* MountProgressionMgr::GetCatalogEntry(uint32 spellId) const
{
    auto it = _catalog.find(spellId);
    return it != _catalog.end() ? &it->second : nullptr;
}

uint32 MountProgressionMgr::XPToNextLevel(MountRarity rarity, uint16 level) const
{
    if (level >= _maxLevel)
        return 0;
    uint32 base = _xpBase[static_cast<size_t>(rarity)];
    return base * static_cast<uint32>(level) * static_cast<uint32>(level);
}

void MountProgressionMgr::LoadPlayerState(Player* player)
{
    if (!player)
        return;

    uint32 guidLow = player->GetGUID().GetCounter();

    QueryResult result = CharacterDatabase.Query(
        "SELECT spell_id, mount_level, mount_xp "
        "FROM character_mount_progress WHERE guid = {}",
        guidLow);

    std::lock_guard<std::mutex> lock(_stateMutex);
    auto& bucket = _progress[guidLow];
    bucket.clear();

    if (result)
    {
        do
        {
            Field* f = result->Fetch();
            MountProgress mp;
            mp.spellId = f[0].Get<uint32>();
            mp.level   = f[1].Get<uint16>();
            mp.xp      = f[2].Get<uint32>();
            mp.dirty   = false;
            bucket.emplace(mp.spellId, mp);
        } while (result->NextRow());
    }

    if (_debug)
        LOG_INFO("module", "mod-mount-progression: loaded {} mount progress rows "
                 "for guid {}", bucket.size(), guidLow);
}

void MountProgressionMgr::SavePlayerState(Player* player)
{
    if (!player)
        return;

    uint32 guidLow = player->GetGUID().GetCounter();

    std::lock_guard<std::mutex> lock(_stateMutex);
    auto it = _progress.find(guidLow);
    if (it == _progress.end())
        return;

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    uint64 now = static_cast<uint64>(::time(nullptr));
    uint32 saved = 0;

    for (auto& kv : it->second)
    {
        uint32 spellId = kv.first;
        MountProgress& mp = kv.second;
        if (!mp.dirty)
            continue;

        trans->Append(
            "INSERT INTO character_mount_progress "
            "(guid, spell_id, mount_level, mount_xp, updated_at) "
            "VALUES ({}, {}, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE "
            "mount_level = VALUES(mount_level), "
            "mount_xp = VALUES(mount_xp), "
            "updated_at = VALUES(updated_at)",
            guidLow, spellId, mp.level, mp.xp, now);

        mp.dirty = false;
        ++saved;
    }

    if (saved)
        CharacterDatabase.CommitTransaction(trans);

    if (_debug && saved)
        LOG_INFO("module", "mod-mount-progression: saved {} dirty rows for guid {}",
                 saved, guidLow);
}

void MountProgressionMgr::UnloadPlayerState(ObjectGuid guid)
{
    uint32 guidLow = guid.GetCounter();
    std::lock_guard<std::mutex> lock(_stateMutex);
    _progress.erase(guidLow);
    _active.erase(guidLow);
    _tickState.erase(guidLow);
}

MountProgress* MountProgressionMgr::GetOrCreateProgress(Player* player, uint32 spellId)
{
    if (!player)
        return nullptr;

    uint32 guidLow = player->GetGUID().GetCounter();
    std::lock_guard<std::mutex> lock(_stateMutex);
    auto& bucket = _progress[guidLow];
    auto it = bucket.find(spellId);
    if (it != bucket.end())
        return &it->second;

    MountProgress mp;
    mp.spellId = spellId;
    mp.level   = 1;
    mp.xp      = 0;
    mp.dirty   = true;
    auto result = bucket.emplace(spellId, mp);
    return &result.first->second;
}

MountProgress const* MountProgressionMgr::GetProgress(Player* player, uint32 spellId) const
{
    if (!player)
        return nullptr;

    uint32 guidLow = player->GetGUID().GetCounter();
    std::lock_guard<std::mutex> lock(_stateMutex);
    auto it = _progress.find(guidLow);
    if (it == _progress.end())
        return nullptr;
    auto mit = it->second.find(spellId);
    return mit != it->second.end() ? &mit->second : nullptr;
}

std::vector<MountProgress> MountProgressionMgr::GetAllProgress(Player* player) const
{
    std::vector<MountProgress> out;
    if (!player)
        return out;

    uint32 guidLow = player->GetGUID().GetCounter();
    std::lock_guard<std::mutex> lock(_stateMutex);
    auto it = _progress.find(guidLow);
    if (it == _progress.end())
        return out;

    out.reserve(it->second.size());
    for (auto const& kv : it->second)
        out.push_back(kv.second);
    return out;
}

void MountProgressionMgr::SetActiveMount(Player* player, uint32 spellId)
{
    if (!player)
        return;
    uint32 guidLow = player->GetGUID().GetCounter();
    std::lock_guard<std::mutex> lock(_stateMutex);
    _active[guidLow] = spellId;
}

uint32 MountProgressionMgr::GetActiveMount(Player* player) const
{
    if (!player)
        return 0;
    uint32 guidLow = player->GetGUID().GetCounter();
    std::lock_guard<std::mutex> lock(_stateMutex);
    auto it = _active.find(guidLow);
    return it != _active.end() ? it->second : 0;
}

void MountProgressionMgr::ClearActiveMount(ObjectGuid guid)
{
    uint32 guidLow = guid.GetCounter();
    std::lock_guard<std::mutex> lock(_stateMutex);
    _active.erase(guidLow);
}

uint32 MountProgressionMgr::KillXpForRank(Creature const* killed) const
{
    if (!killed)
        return 0;
    if (killed->isWorldBoss())
        return _killXpBoss;
    if (killed->isElite())
        return _killXpElite;
    return _killXpNormal;
}

void MountProgressionMgr::OnCreatureKill(Player* player, Creature* killed)
{
    if (!_enabled || !player || !killed)
        return;

    uint32 activeSpell = GetActiveMount(player);
    if (!activeSpell)
        return;

    CatalogEntry const* entry = GetCatalogEntry(activeSpell);
    if (!entry || entry->type != MountType::Predator)
        return;

    int32 delta = static_cast<int32>(killed->GetLevel())
                - static_cast<int32>(player->GetLevel());
    if (delta < _killMinLevelDelta)
        return;

    uint32 xp = KillXpForRank(killed);
    if (!xp)
        return;

    MountProgress* mp = GetOrCreateProgress(player, activeSpell);
    if (!mp)
        return;

    AwardXP(player, mp, xp, entry);
}

CatalogEntry const* MountProgressionMgr::ActivateMount(Player* player, uint32 spellId)
{
    if (!_enabled || !player)
        return nullptr;
    CatalogEntry const* entry = GetCatalogEntry(spellId);
    if (!entry)
        return nullptr;

    uint32 prev = GetActiveMount(player);
    SetActiveMount(player, spellId);
    MountProgress* mp = GetOrCreateProgress(player, spellId);

    if (_debug)
        LOG_INFO("module",
                 "mod-mount-progression: guid {} active mount -> {} ({})",
                 player->GetGUID().GetCounter(), spellId, entry->displayName);

    ApplyMountBuff(player, entry, mp ? mp->level : 1);

    if (prev != spellId && _announceOnCast)
        AnnounceActiveMount(player, entry);
    return entry;
}

void MountProgressionMgr::OnSpellCast(Player* player, SpellInfo const* info)
{
    if (!_enabled || !player || !info)
        return;

    if (ActivateMount(player, info->Id))
        return;

    // Non-mount cast: feed arcane or mechanical depending on spell effect.
    bool isCrafting = false;
    for (auto const& eff : info->GetEffects())
    {
        if (eff.Effect == SPELL_EFFECT_CREATE_ITEM)
        {
            isCrafting = true;
            break;
        }
    }

    if (isCrafting)
        AwardActiveIfType(player, MountType::Mechanical, _craftXPPerCraft);
    else if (_castXPPerCast > 0)
        AwardActiveIfType(player, MountType::Arcane, _castXPPerCast);
}

void MountProgressionMgr::OnPlayerTick(Player* player, uint32 diff)
{
    if (!_enabled || !player)
        return;

    uint32 spellId = GetActiveMount(player);
    if (!spellId)
        return;
    CatalogEntry const* entry = GetCatalogEntry(spellId);
    if (!entry || entry->type != MountType::Stamina)
        return;
    if (_yardsPerXP == 0)
        return;

    uint32 guidLow = player->GetGUID().GetCounter();
    float curX = player->GetPositionX();
    float curY = player->GetPositionY();
    float curZ = player->GetPositionZ();

    uint32 xp = 0;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        PlayerTickState& state = _tickState[guidLow];
        state.accumMs += diff;
        if (state.accumMs < 1000)
            return;
        state.accumMs = 0;

        if (!state.hasLastPos)
        {
            state.lastX = curX;
            state.lastY = curY;
            state.lastZ = curZ;
            state.hasLastPos = true;
            return;
        }

        float dx = curX - state.lastX;
        float dy = curY - state.lastY;
        float dz = curZ - state.lastZ;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        state.lastX = curX;
        state.lastY = curY;
        state.lastZ = curZ;

        if (dist > static_cast<float>(_maxYardsPerTick))
            return;

        state.accumYards += dist;
        float perXP = static_cast<float>(_yardsPerXP);
        xp = static_cast<uint32>(state.accumYards / perXP);
        if (!xp)
            return;
        state.accumYards -= static_cast<float>(xp) * perXP;
    }

    MountProgress* mp = GetOrCreateProgress(player, spellId);
    if (mp)
        AwardXP(player, mp, xp, entry);
}

void MountProgressionMgr::OnPlayerLoot(Player* player, ObjectGuid lootguid)
{
    if (!_enabled || !player)
        return;
    if (!lootguid.IsAnyTypeGameObject())
        return;
    AwardActiveIfType(player, MountType::Mechanical, _gatherXPPerLoot);
}

void MountProgressionMgr::OnPlayerAreaChange(Player* player, uint32 /*oldArea*/,
                                             uint32 newArea)
{
    if (!_enabled || !player || !newArea)
        return;

    uint32 spellId = GetActiveMount(player);
    if (!spellId)
        return;
    CatalogEntry const* entry = GetCatalogEntry(spellId);
    if (!entry || entry->type != MountType::Agility)
        return;
    if (_areaXPPerTransition == 0)
        return;

    uint32 guidLow = player->GetGUID().GetCounter();
    bool isNew = false;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        auto& visited = _tickState[guidLow].visitedAreas;
        isNew = visited.insert(newArea).second;
    }
    if (!isNew)
        return;

    MountProgress* mp = GetOrCreateProgress(player, spellId);
    if (mp)
        AwardXP(player, mp, _areaXPPerTransition, entry);
}

bool MountProgressionMgr::AwardActiveIfType(Player* player, MountType required,
                                            uint32 amount)
{
    if (!player || amount == 0)
        return false;
    uint32 spellId = GetActiveMount(player);
    if (!spellId)
        return false;
    CatalogEntry const* entry = GetCatalogEntry(spellId);
    if (!entry || entry->type != required)
        return false;
    MountProgress* mp = GetOrCreateProgress(player, spellId);
    if (!mp)
        return false;
    AwardXP(player, mp, amount, entry);
    return true;
}

void MountProgressionMgr::AnnounceActiveMount(Player* player,
                                              CatalogEntry const* entry)
{
    if (!player || !entry || !player->GetSession())
        return;
    MountProgress const* mp = GetProgress(player, entry->spellId);
    uint16 level = mp ? mp->level : 1;
    uint32 xp = mp ? mp->xp : 0;
    uint32 need = XPToNextLevel(entry->rarity, level);

    ChatHandler ch(player->GetSession());
    if (need == 0)
        ch.PSendSysMessage(
            "|cff40ff80[坐骑]|r 当前：|cffffd100{}|r（{} {}）- 等级 {}（满级）- 经验来源：{}",
            entry->displayName, RarityName(entry->rarity),
            TypeName(entry->type), level, XPSourceName(entry->type));
    else
        ch.PSendSysMessage(
            "|cff40ff80[坐骑]|r 当前：|cffffd100{}|r（{} {}）- 等级 {}，经验 {}/{} - 经验来源：{}",
            entry->displayName, RarityName(entry->rarity),
            TypeName(entry->type), level, xp, need, XPSourceName(entry->type));
}

void MountProgressionMgr::AwardXP(Player* player, MountProgress* mp, uint32 amount,
                                  CatalogEntry const* entry)
{
    if (!mp || !entry || amount == 0)
        return;

    if (mp->level >= _maxLevel)
        return;

    uint32 leveledUpTo = 0;
    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        mp->xp += amount;
        mp->dirty = true;

        while (mp->level < _maxLevel)
        {
            uint32 need = XPToNextLevel(entry->rarity, mp->level);
            if (need == 0 || mp->xp < need)
                break;
            mp->xp -= need;
            mp->level += 1;
            leveledUpTo = mp->level;
        }

        if (mp->level >= _maxLevel)
            mp->xp = 0;
    }

    if (leveledUpTo && player->GetSession())
    {
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cff40ff40你的 {} 达到了坐骑等级 {}！|r",
            entry->displayName, leveledUpTo);
    }

    if (leveledUpTo && GetActiveMount(player) == entry->spellId)
        ApplyMountBuff(player, entry, static_cast<uint16>(leveledUpTo));
}

bool MountProgressionMgr::AwardActiveMountXP(Player* player, uint32 amount)
{
    if (!player || amount == 0)
        return false;

    uint32 spellId = GetActiveMount(player);
    if (!spellId)
        return false;

    CatalogEntry const* entry = GetCatalogEntry(spellId);
    if (!entry)
        return false;

    MountProgress* mp = GetOrCreateProgress(player, spellId);
    if (!mp)
        return false;

    AwardXP(player, mp, amount, entry);
    return true;
}

bool MountProgressionMgr::SetActiveMountLevel(Player* player, uint16 level)
{
    if (!player)
        return false;

    uint32 spellId = GetActiveMount(player);
    if (!spellId)
        return false;

    CatalogEntry const* entry = GetCatalogEntry(spellId);
    if (!entry)
        return false;

    MountProgress* mp = GetOrCreateProgress(player, spellId);
    if (!mp)
        return false;

    if (level < 1)
        level = 1;
    if (level > _maxLevel)
        level = _maxLevel;

    {
        std::lock_guard<std::mutex> lock(_stateMutex);
        mp->level = level;
        mp->xp = 0;
        mp->dirty = true;
    }

    ApplyMountBuff(player, entry, level);
    return true;
}

double MountProgressionMgr::CurveFraction(uint16 level) const
{
    uint16 lvl = level < 1 ? 1 : level;
    if (lvl > _maxLevel)
        lvl = _maxLevel;

    switch (_buffLevelCurve)
    {
        case 1:  // stepped quartiles
        {
            double q = static_cast<double>(_maxLevel) / 4.0;
            if (lvl < static_cast<uint16>(std::ceil(q * 1)))
                return 0.25;
            if (lvl < static_cast<uint16>(std::ceil(q * 2)))
                return 0.50;
            if (lvl < static_cast<uint16>(std::ceil(q * 3)))
                return 0.75;
            return 1.0;
        }
        case 2:  // quadratic
        {
            double f = static_cast<double>(lvl) / _maxLevel;
            return f * f;
        }
        default:  // 0 — linear
            return static_cast<double>(lvl) / _maxLevel;
    }
}

uint32 MountProgressionMgr::ComputeBuffMagnitude(CatalogEntry const* entry,
                                                 uint16 level) const
{
    if (!entry)
        return 0;

    uint32 ceiling = _buffCeiling[static_cast<size_t>(entry->rarity)];
    double frac = CurveFraction(level);

    // Mode 0 (spec-literal % of base stat) is config-exposed but not yet
    // implemented — falls through to mode 1. See SLICE_3_PLAN.md §5.1.
    double magnitude = static_cast<double>(ceiling) * frac;
    if (magnitude < 0.0)
        magnitude = 0.0;
    return static_cast<uint32>(std::round(magnitude));
}

void MountProgressionMgr::ApplyMountBuff(Player* player,
                                         CatalogEntry const* entry,
                                         uint16 level)
{
    // Skip dead/ghost players: the carrier spell is non-passive and not flagged
    // ALLOW_DEAD_TARGET, so AddAura() returns null for them (which logged a misleading
    // "spell_dbc row missing or immune?" warning, e.g. from OnPlayerReleasedGhost). The buff
    // re-applies naturally on resurrect/remount.
    if (!_enabled || !player || !entry || !player->IsAlive())
        return;

    size_t typeIdx = static_cast<size_t>(entry->type);
    if (typeIdx >= static_cast<size_t>(MountType::MAX))
        return;

    uint32 carrierId = _carrierSpell[typeIdx];
    if (!carrierId)
        return;

    int32 amount = static_cast<int32>(ComputeBuffMagnitude(entry, level));

    RemoveMountBuff(player);

    Aura* aura = player->AddAura(carrierId, player);
    if (!aura)
    {
        LOG_WARN("module",
                 "mod-mount-progression: AddAura({}) failed for guid {} "
                 "(mount {}, type {}); spell_dbc row missing or immune?",
                 carrierId, player->GetGUID().GetCounter(),
                 entry->spellId, TypeName(entry->type));
        return;
    }

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (AuraEffect* eff = aura->GetEffect(i))
            eff->ChangeAmount(amount);

    if (_debug)
        LOG_INFO("module",
                 "mod-mount-progression: applied carrier {} ({}) amount={} "
                 "to guid {} for mount {} lvl {}",
                 carrierId, TypeName(entry->type), amount,
                 player->GetGUID().GetCounter(), entry->spellId, level);
}

void MountProgressionMgr::RemoveMountBuff(Player* player)
{
    if (!player)
        return;
    for (size_t i = 0; i < static_cast<size_t>(MountType::MAX); ++i)
        if (_carrierSpell[i])
            player->RemoveAurasDueToSpell(_carrierSpell[i]);
}

void MountProgressionMgr::SaveActiveMountToDB(Player* player)
{
    if (!player)
        return;

    uint32 guidLow = player->GetGUID().GetCounter();
    uint32 spellId = GetActiveMount(player);

    if (!spellId)
    {
        CharacterDatabase.Execute(
            "DELETE FROM character_mount_active WHERE guid = {}", guidLow);
        return;
    }

    uint64 now = static_cast<uint64>(::time(nullptr));
    CharacterDatabase.Execute(
        "INSERT INTO character_mount_active (guid, spell_id, last_active_time) "
        "VALUES ({}, {}, {}) "
        "ON DUPLICATE KEY UPDATE "
        "spell_id = VALUES(spell_id), "
        "last_active_time = VALUES(last_active_time)",
        guidLow, spellId, now);
}

void MountProgressionMgr::LoadActiveMountFromDB(Player* player)
{
    if (!_enabled || !player)
        return;

    uint32 guidLow = player->GetGUID().GetCounter();
    QueryResult result = CharacterDatabase.Query(
        "SELECT spell_id, last_active_time FROM character_mount_active "
        "WHERE guid = {}",
        guidLow);
    if (!result)
        return;

    Field* f = result->Fetch();
    uint32 spellId = f[0].Get<uint32>();
    uint64 lastActive = f[1].Get<uint64>();

    CatalogEntry const* entry = GetCatalogEntry(spellId);
    if (!entry)
    {
        CharacterDatabase.Execute(
            "DELETE FROM character_mount_active WHERE guid = {}", guidLow);
        return;
    }

    uint64 now = static_cast<uint64>(::time(nullptr));
    if (_offlineGraceSeconds > 0 &&
        now - lastActive > _offlineGraceSeconds)
    {
        CharacterDatabase.Execute(
            "DELETE FROM character_mount_active WHERE guid = {}", guidLow);
        if (_debug)
            LOG_INFO("module",
                     "mod-mount-progression: guid {} offline {}s > grace {}s, "
                     "carrier not reapplied",
                     guidLow, static_cast<uint64>(now - lastActive),
                     _offlineGraceSeconds);
        return;
    }

    SetActiveMount(player, spellId);
    MountProgress const* mp = GetProgress(player, spellId);
    uint16 level = mp ? mp->level : 1;
    ApplyMountBuff(player, entry, level);
}

}  // namespace mod_mount_progression
