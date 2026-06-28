#include "TerrorZonesMgr.h"

#include "Player.h"
#include "ScriptMgr.h"

using namespace mod_terror_zones;

namespace
{
    class TerrorZones_PlayerScript : public PlayerScript
    {
    public:
        TerrorZones_PlayerScript() : PlayerScript("TerrorZones_PlayerScript") {}

        void OnPlayerLogin(Player* player) override
        {
            TerrorZonesMgr::Instance().OnPlayerLogin(player);
        }

        void OnPlayerLogout(Player* player) override
        {
            if (!player)
                return;
            auto& mgr = TerrorZonesMgr::Instance();
            mgr.FlushPlayerPref(player);
            mgr.UnloadPlayerPref(player->GetGUID());
        }

        void OnPlayerSave(Player* player) override
        {
            TerrorZonesMgr::Instance().FlushPlayerPref(player);
        }

        void OnPlayerUpdateZone(Player* player, uint32 newZone,
                                uint32 /*newArea*/) override
        {
            TerrorZonesMgr::Instance().OnPlayerUpdateZone(player, newZone);
        }
    };
}

void AddTerrorZonesPlayerScripts()
{
    new TerrorZones_PlayerScript();
}
