// Slice 8 — UnitScript hooks. Split from TerrorZonesCreatureScripts.cpp
// because UnitScript is a distinct ScriptObject base class; the one-
// class-per-file convention used elsewhere in the module calls for
// its own file.

#include "TerrorZonesMgr.h"

#include "ScriptMgr.h"
#include "Unit.h"

using namespace mod_terror_zones;

namespace
{
    class TerrorZones_UnitScript : public UnitScript
    {
    public:
        TerrorZones_UnitScript()
            : UnitScript("TerrorZones_UnitScript") {}

        // OnDamage fires on every damage dispatch — melee, spell, DoT
        // ticks all pass through here with the attacker in hand. We
        // scale outgoing damage for eligible empowered-zone creatures.
        void OnDamage(Unit* attacker, Unit* victim,
                      uint32& damage) override
        {
            TerrorZonesMgr::Instance().OnUnitDealDamage(attacker, victim,
                                                         damage);
        }
    };
}

void AddTerrorZonesUnitScripts()
{
    new TerrorZones_UnitScript();
}
