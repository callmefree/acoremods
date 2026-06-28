#pragma once

#include "DynamicAHTypes.h"
#include "SkillDiscovery.h"
#include "DBCStores.h"
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "DatabaseEnv.h"

namespace ModDynamicAH
{

    // Small accessor around difficulty “caches”
    class DynamicAHDifficulty
    {
    public:
        static void Build();                                  // call once on config load
        static uint16 MaxReqSkillForItem(uint32 itemId);      // from reagents across all professions
        static uint8 MinCreatureLevelDropping(uint32 itemId); // min creature level that drops the item

    private:
        static std::unordered_map<uint32, uint16> &SkillMap();
        static std::unordered_map<uint32, uint8> &CreatureLvlMap();
    };

} // namespace ModDynamicAH
