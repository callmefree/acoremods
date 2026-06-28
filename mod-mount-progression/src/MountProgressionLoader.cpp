#include "MountProgressionMgr.h"

#include "DBCStores.h"
#include "DBCfmt.h"
#include "Log.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"

void AddMountProgressionPlayerScripts();
void AddMountProgressionCommandScripts();

namespace
{
    constexpr char const* CARRIER_TABLE = "mount_progression_carrier_spells";

    class Mount_WorldScript : public WorldScript
    {
    public:
        Mount_WorldScript()
            : WorldScript("Mount_WorldScript",
                          { WORLDHOOK_ON_AFTER_CONFIG_LOAD,
                            WORLDHOOK_ON_AFTER_LOAD_DBC_STORES,
                            WORLDHOOK_ON_STARTUP }) {}

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            mod_mount_progression::MountProgressionMgr::Instance().LoadConfig();
        }

        void OnAfterLoadDBCStores() override
        {
            // Merge the module-owned carrier spells into sSpellStore
            // before SpellMgr::LoadSpellInfoStore() builds mSpellInfoMap.
            // Runs inside the OnAfterLoadDBCStores core hook, which fires
            // in World.cpp between LoadDBCStores() and LoadSpellInfoStore().
            sSpellStore.LoadFromDB(CARRIER_TABLE, SpellEntryfmt);
            LOG_INFO("module",
                     "mod-mount-progression: merged carrier spells from "
                     "`{}` into sSpellStore.", CARRIER_TABLE);
        }

        void OnStartup() override
        {
            mod_mount_progression::MountProgressionMgr::Instance().LoadCatalog();
        }
    };

    class Mount_UnitScript : public UnitScript
    {
    public:
        Mount_UnitScript()
            : UnitScript("Mount_UnitScript", true,
                         { UNITHOOK_ON_AURA_BUILD_UPDATE_PACKET }) {}

        // Fires inside AuraApplication::BuildUpdatePacket. If the aura is
        // one of our carriers, rewrite the outgoing spell ID to a
        // client-known icon-donor spell so the buff tray renders a real
        // icon and tooltip. The server-side aura is unchanged.
        void OnAuraBuildUpdatePacket(Aura const* aura, uint32& spellId) override
        {
            if (!aura)
                return;
            if (uint32 donor = mod_mount_progression::MountProgressionMgr::
                               Instance().GetIconDonor(aura->GetId()))
                spellId = donor;
        }
    };
}

void Addmod_mount_progressionScripts()
{
    LOG_INFO("module", "mod-mount-progression: registering scripts.");
    new Mount_WorldScript();
    new Mount_UnitScript();
    AddMountProgressionPlayerScripts();
    AddMountProgressionCommandScripts();
}
