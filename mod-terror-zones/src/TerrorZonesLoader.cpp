#include "TerrorZonesMgr.h"

#include "Log.h"
#include "ScriptMgr.h"

void AddTerrorZonesPlayerScripts();
void AddTerrorZonesCommandScripts();
void AddTerrorZonesCreatureScripts();
void AddTerrorZonesRewardScripts();
void AddTerrorZonesUnitScripts();

namespace
{
    class TerrorZones_WorldScript : public WorldScript
    {
    public:
        TerrorZones_WorldScript()
            : WorldScript("TerrorZones_WorldScript",
                          { WORLDHOOK_ON_AFTER_CONFIG_LOAD,
                            WORLDHOOK_ON_STARTUP,
                            WORLDHOOK_ON_UPDATE }) {}

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            mod_terror_zones::TerrorZonesMgr::Instance().LoadConfig();
        }

        void OnStartup() override
        {
            mod_terror_zones::TerrorZonesMgr::Instance().InitializeOnStartup();
        }

        void OnUpdate(uint32 diff) override
        {
            mod_terror_zones::TerrorZonesMgr::Instance().OnUpdate(diff);
        }
    };
}

void Addmod_terror_zonesScripts()
{
    LOG_INFO("module", "mod-terror-zones: registering scripts.");
    new TerrorZones_WorldScript();
    AddTerrorZonesPlayerScripts();
    AddTerrorZonesCommandScripts();
    AddTerrorZonesCreatureScripts();
    AddTerrorZonesRewardScripts();
    AddTerrorZonesUnitScripts();
}
