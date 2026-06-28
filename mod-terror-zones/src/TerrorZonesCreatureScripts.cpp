#include "TerrorZonesMgr.h"

#include "Creature.h"
#include "ScriptMgr.h"

using namespace mod_terror_zones;

namespace
{
    class TerrorZones_AllCreatureScript : public AllCreatureScript
    {
    public:
        TerrorZones_AllCreatureScript()
            : AllCreatureScript("TerrorZones_AllCreatureScript") {}

        void OnBeforeCreatureSelectLevel(CreatureTemplate const* /*cinfo*/,
                                         Creature* creature,
                                         uint8& level) override
        {
            TerrorZonesMgr::Instance().OnBeforeCreatureSelectLevel(creature,
                                                                    level);
        }

        // Slice 8 — post-SelectLevel HP mult. Fires right after the
        // creature's base stats (HP, mana, weapon damage) are set from
        // CreatureBaseStats. We multiply MaxHealth in place for
        // eligible empowered-zone creatures.
        void OnCreatureSelectLevel(CreatureTemplate const* /*cinfo*/,
                                    Creature* creature) override
        {
            TerrorZonesMgr::Instance().OnAfterCreatureSelectLevel(creature);
        }
    };
}

void AddTerrorZonesCreatureScripts()
{
    new TerrorZones_AllCreatureScript();
}
