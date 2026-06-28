#pragma once

#include "DynamicAHTypes.h"
#include "DynamicAHScarcity.h"
#include "DynamicAHVendor.h"
#include "DynamicAHPricing.h"
#include "ProfessionMats.h" // your existing mat tables
#include "DynamicAHSelection.h"

class Player;

namespace ModDynamicAH
{
    struct ModuleState;

    // Runtime, per-cycle caches & config are owned by world; planner only builds queues.
    struct PlannerConfig
    {
        // general
        bool enableSeller = true;
        uint32 minPriceCopper = 10000;

        // categories multipliers
        double mulDust = 1.00;
        double mulEssence = 1.25;
        double mulShard = 2.00;
        double mulElemental = 3.00;
        double mulRareRaw = 3.00;

        // scarcity
        bool scarcityEnabled = true;
        double scarcityPriceBoostMax = 0.30;
        uint32 scarcityPerItemPerTickCap = 1;

        // vendor
        double vendorMinMarkup = 0.25;
        bool vendorConsiderBuyPrice = true;

        // stacks
        uint32 stDefault = 20, stCloth = 20, stOre = 20, stBar = 20, stHerb = 20, stLeather = 20, stDust = 20, stGem = 20, stStone = 20, stMeat = 20, stBandage = 20, stPotion = 5, stInk = 10, stPigment = 20, stFish = 20;
        uint32 stacksLow = 2, stacksMid = 3, stacksHigh = 2;

        // context planner
        bool contextEnabled = true;
        uint32 contextMaxPerBracket = 4;
        double contextWeightBoost = 1.5;
        bool contextSkipVendor = true;

        // random selection
        bool blockTrashAndCommon = true;
        bool allowQuality[6] = {false, false, true, true, true, false};
        std::unordered_set<uint32> whitelist;
        uint32 maxRandomPerCycle = 50;

        // economy
        double avgGoldPerQuest = 10.0;
        uint32 questsPerFamily[(size_t)Family::COUNT] = {0};

        // caps (hard ceilings on auctions queued per cycle; 0 = unlimited for that bucket)
        bool capsEnabled = true;
        uint32 capTotalPerCycle = 150;
        uint32 capPerHouse[3] = {80, 80, 120};         // [Alliance, Horde, Neutral]
        uint32 capPerFamily[(size_t)Family::COUNT] = {0};
    };

    class DynamicAHPlanner
    {
    public:
        void ResetTick(uint32 onlineCount);
        void BuildScarcityCache(ModuleState const &s);

        void BuildContextPlan(PlannerConfig const &cfg);
        void BuildRandomPlan(PlannerConfig const &cfg);
        // pricing helpers
        void PriceWithPolicies(PlannerConfig const &cfg, Family fam, uint32 itemId, ItemTemplate const *tmpl,
                               AuctionHouseId house, uint32 &outStart, uint32 &outBuy) const;
        static uint32 ClampToStackable(ItemTemplate const *tmpl, uint32 desired);
        PostQueue &Queue() { return _queue; }

    private:
        static double Jitter(uint32 itemId);

        // post cap per-item per tick
    public:
        uint32 ScarcityCount(uint32 itemId, AuctionHouseId house) const;
        // Reserve one auction slot for (house, family) against the per-cycle caps. Returns false
        // (and reserves nothing) if any applicable cap would be exceeded. Reset each cycle via
        // ResetCycleCaps().
        bool TryPlanOnce(AuctionHouseId house, Family fam);
        void ResetCycleCaps(PlannerConfig const &cfg);

    public:
        PostQueue _queue;
        std::unordered_map<uint64, uint32> _perTickPlanCap; // (house<<32)|itemId -> count this tick
        DynamicAHScarcity _scarcity;
        uint32 _online = 0;

        // Per-cycle cap accounting (limits copied from PlannerConfig in ResetCycleCaps).
        bool _capsEnabled = false;
        uint32 _capTotal = 0;
        uint32 _capHouse[3] = {0, 0, 0};
        uint32 _capFamily[(size_t)Family::COUNT] = {0};
        uint32 _plTotal = 0;
        uint32 _plHouse[3] = {0, 0, 0};
        uint32 _plFamily[(size_t)Family::COUNT] = {0};

        // category sets (built once)
        static std::unordered_set<uint32> &EssenceSet();
        static std::unordered_set<uint32> &ShardSet();
        static std::unordered_set<uint32> &ElementalSet();
        static std::unordered_set<uint32> &RareRawSet();

        static double CategoryMul(PlannerConfig const &cfg, uint32 itemId);
        static void InitCategorySetsOnce();
    };

} // namespace ModDynamicAH
