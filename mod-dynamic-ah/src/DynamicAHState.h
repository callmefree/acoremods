#pragma once

#include <cstdint>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>
#include "DynamicAHTypes.h"

namespace ModDynamicAH
{
    struct ModuleState
    {
        // toggles & timing
        bool enableSeller = true;
        bool dryRun = true;
        uint32_t intervalMin = 30;
        uint32_t maxRandomPerCycle = 50;
        bool loopEnabled = true;

        // pricing & selection filters
        uint32_t minPriceCopper = 10000;
        bool blockTrashAndCommon = true;
        bool allowQuality[6] = {false, false, true, true, true, false};
        std::unordered_set<uint32_t> whiteAllow;

        // owners (character low GUIDs)
        uint32_t ownerAlliance = 0;
        uint32_t ownerHorde = 0;
        uint32_t ownerNeutral = 0;

        // context planning
        bool contextEnabled = true;
        uint32_t contextMaxPerBracket = 4;
        float contextWeightBoost = 1.5f;
        bool contextSkipVendor = true;

        // scarcity
        bool scarcityEnabled = true;
        float scarcityPriceBoostMax = 0.30f;
        uint32_t scarcityPerItemPerTickCap = 1;

        // vendor floor
        float vendorMinMarkup = 0.25f; // 25%
        bool vendorConsiderBuyPrice = true;
        bool neverBuyAboveVendorBuyPrice = true;

        // stacks & categories
        uint32_t stDefault = 20;
        uint32_t stCloth = 20, stOre = 20, stBar = 20, stHerb = 20, stLeather = 20, stDust = 20, stGem = 20, stStone = 20, stMeat = 20, stBandage = 20, stPotion = 5, stInk = 10, stPigment = 20, stFish = 20;
        uint32_t stacksLow = 2, stacksMid = 3, stacksHigh = 2;

        // multipliers for pricing
        float mulDust = 1.0f, mulEssence = 1.25f, mulShard = 2.0f, mulRareRaw = 3.0f;
        float mulElemental = 3.0f;

        // economy
        double avgGoldPerQuest = 10.0;
        uint32_t questsPerFamily[(size_t)Family::COUNT] = {0};

        // debug
        bool debugContextLogs = false;

        // runtime per-cycle
        struct Cycle
        {
            uint32_t onlineCount = 0;
            void Clear() { onlineCount = 0; }
        } cycle;

        // per-tick/post plan counts and caches
        std::unordered_map<uint32_t, uint32_t> tickPlanCounts; // itemId -> planned count this tick
        std::unordered_set<uint32_t> vendorSoldCache;

        // simple timing for loop
        uint64_t nextRunMs = 0;

        // posting queue shared across the module
        PostQueue postQueue;

        // basic caps we expose to commands & planner
        struct Caps
        {
            bool enabled = true;
            uint32_t totalPerCycleLimit = 150;

            uint32_t perHouseLimit[3] = {80, 80, 120};
            uint32_t perHousePlanned[3] = {0, 0, 0};

            uint32_t familyLimit[(size_t)Family::COUNT] = {0};
            uint32_t familyPlanned[(size_t)Family::COUNT] = {0};

            void ResetCounts()
            {
                perHousePlanned[0] = perHousePlanned[1] = perHousePlanned[2] = 0;
                for (size_t i = 0; i < (size_t)Family::COUNT; ++i)
                    familyPlanned[i] = 0;
            }

            void InitDefaults()
            {
                ResetCounts();
            }
        } caps;
    };
} // namespace ModDynamicAH
