#include "StateGraph.h"
#include "StateGraphCache.h"

#include "Log.h"
#include "ScriptMgr.h"

void AddLivingWorldCommandScripts();
void AddLivingWorldStormwindScripts();

namespace
{

class LivingWorld_WorldScript : public WorldScript
{
public:
    LivingWorld_WorldScript()
        : WorldScript("LivingWorld_WorldScript",
                      { WORLDHOOK_ON_AFTER_CONFIG_LOAD,
                        WORLDHOOK_ON_STARTUP,
                        WORLDHOOK_ON_SHUTDOWN }) {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LivingWorld::StateGraph::StateGraphMgr::Instance().LoadConfig();
    }

    void OnStartup() override
    {
        LivingWorld::StateGraph::Initialize();
    }

    void OnShutdown() override
    {
        LivingWorld::StateGraph::Shutdown();
    }
};

}  // namespace

void Addmod_living_worldScripts()
{
    LOG_INFO("module", "mod-living-world: registering scripts.");
    new LivingWorld_WorldScript();
    AddLivingWorldCommandScripts();
    AddLivingWorldStormwindScripts();
}
