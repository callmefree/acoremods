#pragma once

#include "DynamicAHTypes.h"
#include "DatabaseEnv.h"
#include "ObjectMgr.h"

namespace ModDynamicAH
{

    struct SelectionConfig
    {
        bool blockTrashAndCommon = true;
        bool allowQuality[6] = {false, false, true, true, true, false}; // Poor..Legendary
        std::unordered_set<uint32> whitelist;                           // allow specific itemIds (e.g. white)
        uint32 maxRandomPostsPerCycle = 50;
        uint32 minPriceCopper = 10000;
    };

    struct ItemCandidate
    {
        uint32 itemId = 0;
        ItemTemplate const *tmpl = nullptr;
    };

    class DynamicAHSelection
    {
    public:
        static std::vector<ItemCandidate> PickRandomSellables(SelectionConfig const &cfg, uint32 maxCount);
    };

} // namespace ModDynamicAH
