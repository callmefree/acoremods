#include "MountProgressionMgr.h"

#include "Creature.h"
#include "Item.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"

using namespace mod_mount_progression;

namespace
{
    class Mount_PlayerScript : public PlayerScript
    {
    public:
        Mount_PlayerScript() : PlayerScript("Mount_PlayerScript") {}

        void OnPlayerLogin(Player* player) override
        {
            auto& mgr = MountProgressionMgr::Instance();
            mgr.LoadPlayerState(player);
            mgr.LoadActiveMountFromDB(player);
        }

        void OnPlayerLogout(Player* player) override
        {
            if (!player)
                return;
            auto& mgr = MountProgressionMgr::Instance();
            mgr.SavePlayerState(player);
            mgr.SaveActiveMountToDB(player);
            mgr.UnloadPlayerState(player->GetGUID());
        }

        void OnPlayerReleasedGhost(Player* player) override
        {
            if (!player)
                return;
            auto& mgr = MountProgressionMgr::Instance();
            uint32 spellId = mgr.GetActiveMount(player);
            if (!spellId)
                return;
            CatalogEntry const* entry = mgr.GetCatalogEntry(spellId);
            if (!entry)
                return;
            MountProgress const* mp = mgr.GetProgress(player, spellId);
            mgr.ApplyMountBuff(player, entry, mp ? mp->level : 1);
        }

        void OnPlayerSave(Player* player) override
        {
            MountProgressionMgr::Instance().SavePlayerState(player);
        }

        void OnPlayerCreatureKill(Player* player, Creature* killed) override
        {
            MountProgressionMgr::Instance().OnCreatureKill(player, killed);
        }

        void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
        {
            if (!player || !spell)
                return;
            SpellInfo const* info = spell->GetSpellInfo();
            if (!info)
                return;
            MountProgressionMgr::Instance().OnSpellCast(player, info);
        }

        void OnPlayerUpdate(Player* player, uint32 p_time) override
        {
            MountProgressionMgr::Instance().OnPlayerTick(player, p_time);
        }

        void OnPlayerUpdateArea(Player* player, uint32 oldArea, uint32 newArea) override
        {
            MountProgressionMgr::Instance().OnPlayerAreaChange(player, oldArea, newArea);
        }

        void OnPlayerLootItem(Player* player, Item* /*item*/, uint32 /*count*/,
                              ObjectGuid lootguid) override
        {
            MountProgressionMgr::Instance().OnPlayerLoot(player, lootguid);
        }
    };
}

void AddMountProgressionPlayerScripts()
{
    new Mount_PlayerScript();
}
