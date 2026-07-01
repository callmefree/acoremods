#include "ModDynamicAHService.h"
#include "DynamicAHTypes.h"

namespace
{
    static int HouseIndexByName(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        if (s == "a" || s == "alliance")
            return 0;
        if (s == "h" || s == "horde")
            return 1;
        if (s == "n" || s == "neutral")
            return 2;
        return -1;
    }
    static bool ParseFamilyName(std::string s, ModDynamicAH::Family &out)
    {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        using ModDynamicAH::Family;
        if (s == "herb")
            out = Family::Herb;
        else if (s == "ore")
            out = Family::Ore;
        else if (s == "bar")
            out = Family::Bar;
        else if (s == "cloth")
            out = Family::Cloth;
        else if (s == "leather")
            out = Family::Leather;
        else if (s == "dust")
            out = Family::Dust;
        else if (s == "essence")
            out = Family::Essence;
        else if (s == "shard")
            out = Family::Shard;
        else if (s == "elemental")
            out = Family::Elemental;
        else if (s == "stone")
            out = Family::Stone;
        else if (s == "meat")
            out = Family::Meat;
        else if (s == "fish")
            out = Family::Fish;
        else if (s == "gem")
            out = Family::Gem;
        else if (s == "bandage")
            out = Family::Bandage;
        else if (s == "potion")
            out = Family::Potion;
        else if (s == "ink")
            out = Family::Ink;
        else if (s == "pigment")
            out = Family::Pigment;
        else if (s == "other")
            out = Family::Other;
        else
            return false;
        return true;
    }
} // anon

#include "DatabaseEnv.h"
#include "Config.h"
#include "GameTime.h"
#include "Log.h"
#include "WorldSessionMgr.h"
#include "ProfessionMats.h"
#include "DynamicAHPricing.h"
#include "DynamicAHDifficulty.h"
#include "DynamicAHSetup.h"

using namespace ModDynamicAH;

namespace
{
    inline uint64_t NowMs() { return (uint64_t)GameTime::GetGameTimeMS().count(); }
    namespace
    {
        inline PlannerConfig ToPlannerCfg(ModuleState const &s)
        {
            PlannerConfig c;
            // general
            c.enableSeller = s.enableSeller;
            c.minPriceCopper = s.minPriceCopper;

            // multipliers
            c.mulDust = s.mulDust;
            c.mulEssence = s.mulEssence;
            c.mulShard = s.mulShard;
            c.mulElemental = s.mulElemental;
            c.mulRareRaw = s.mulRareRaw;

            // scarcity
            c.scarcityEnabled = s.scarcityEnabled;
            c.scarcityPriceBoostMax = s.scarcityPriceBoostMax;
            c.scarcityPerItemPerTickCap = s.scarcityPerItemPerTickCap;

            // vendor
            c.vendorMinMarkup = s.vendorMinMarkup;
            c.vendorConsiderBuyPrice = s.vendorConsiderBuyPrice;

            // stacks
            c.stDefault = (int32)s.stDefault;
            c.stCloth = (int32)s.stCloth;
            c.stOre = (int32)s.stOre;
            c.stBar = (int32)s.stBar;
            c.stHerb = (int32)s.stHerb;
            c.stLeather = (int32)s.stLeather;
            c.stDust = (int32)s.stDust;
            c.stGem = (int32)s.stGem;
            c.stStone = (int32)s.stStone;
            c.stMeat = (int32)s.stMeat;
            c.stBandage = (int32)s.stBandage;
            c.stPotion = (int32)s.stPotion;
            c.stInk = (int32)s.stInk;
            c.stPigment = (int32)s.stPigment;
            c.stFish = (int32)s.stFish;
            c.stacksLow = s.stacksLow;
            c.stacksMid = s.stacksMid;
            c.stacksHigh = s.stacksHigh;

            // context
            c.contextEnabled = s.contextEnabled;
            c.contextMaxPerBracket = s.contextMaxPerBracket;
            c.contextWeightBoost = s.contextWeightBoost;
            c.contextSkipVendor = s.contextSkipVendor;

            // random selection
            c.blockTrashAndCommon = s.blockTrashAndCommon;
            for (size_t i = 0; i < 6; i++)
                c.allowQuality[i] = s.allowQuality[i];
            c.whitelist = s.whiteAllow;
            c.maxRandomPerCycle = s.maxRandomPerCycle;

            // economy
            c.avgGoldPerQuest = s.avgGoldPerQuest;
            for (size_t i = 0; i < (size_t)Family::COUNT; i++)
                c.questsPerFamily[i] = s.questsPerFamily[i];

            // caps (so the planner actually enforces the configured ceilings)
            c.capsEnabled = s.caps.enabled;
            c.capTotalPerCycle = s.caps.totalPerCycleLimit;
            for (int i = 0; i < 3; i++)
                c.capPerHouse[i] = s.caps.perHouseLimit[i];
            for (size_t i = 0; i < (size_t)Family::COUNT; i++)
                c.capPerFamily[i] = s.caps.familyLimit[i];

            return c;
        }
    } // anonymous namespace

}

Service &Service::Instance()
{
    static Service s;
    return s;
}

void Service::OnConfigLoad()
{
    auto &g = state_;

    g.enableSeller = sConfigMgr->GetOption<bool>(CFG_ENABLE_SELLER, true);
    g.dryRun = sConfigMgr->GetOption<bool>(CFG_DRYRUN, true);
    g.intervalMin = sConfigMgr->GetOption<uint32_t>(CFG_INTERVAL_MIN, 600);
    g.maxRandomPerCycle = sConfigMgr->GetOption<uint32_t>(CFG_MAX_RANDOM, 50);
    g.minPriceCopper = sConfigMgr->GetOption<uint32_t>(CFG_MIN_PRICE, 10000);
    g.blockTrashAndCommon = sConfigMgr->GetOption<bool>(CFG_BLOCK_TRASH, true);
    g.allowQuality[0] = sConfigMgr->GetOption<bool>(CFG_ALLOW_Q_POOR, false);
    g.allowQuality[1] = sConfigMgr->GetOption<bool>(CFG_ALLOW_Q_NORMAL, false);
    g.allowQuality[2] = sConfigMgr->GetOption<bool>(CFG_ALLOW_Q_UNCOMMON, true);
    g.allowQuality[3] = sConfigMgr->GetOption<bool>(CFG_ALLOW_Q_RARE, true);
    g.allowQuality[4] = sConfigMgr->GetOption<bool>(CFG_ALLOW_Q_EPIC, true);
    g.allowQuality[5] = sConfigMgr->GetOption<bool>(CFG_ALLOW_Q_LEGENDARY, false);
    g.neverBuyAboveVendorBuyPrice = sConfigMgr->GetOption<bool>(CFG_NEVER_ABOVE_VENDOR_BUY, true);

    g.ownerAlliance = sConfigMgr->GetOption<uint32_t>(CFG_SELLER_OWNER_ALLI, g.ownerAlliance);
    g.ownerHorde = sConfigMgr->GetOption<uint32_t>(CFG_SELLER_OWNER_HORDE, g.ownerHorde);
    g.ownerNeutral = sConfigMgr->GetOption<uint32_t>(CFG_SELLER_OWNER_NEUT, g.ownerNeutral);

    // Owner GUIDs are not persisted by `.dah setup`, so after a restart they would be lost and
    // posting would silently fail. Recover any unset owner by looking the configured seller
    // character up by name (the characters created by a prior `.dah setup` still exist).
    DynamicAHSetup::ResolveOwnersByName(g);

    g.whiteAllow = ParseCsvU32(sConfigMgr->GetOption<std::string>(CFG_WHITE_ALLOW, ""));

    g.contextEnabled = sConfigMgr->GetOption<bool>(CFG_CONTEXT_ENABLED, true);
    g.contextMaxPerBracket = sConfigMgr->GetOption<uint32_t>(CFG_CONTEXT_MAX_PER_BRACKET, 4u);
    g.contextWeightBoost = sConfigMgr->GetOption<float>(CFG_CONTEXT_WEIGHT_BOOST, 1.5f);
    g.contextSkipVendor = sConfigMgr->GetOption<bool>(CFG_CONTEXT_VENDOR_SKIP, true);

    g.scarcityEnabled = sConfigMgr->GetOption<bool>(CFG_SCARCITY_ENABLED, true);
    g.scarcityPriceBoostMax = sConfigMgr->GetOption<float>(CFG_SCARCITY_PRICE_BOOST_MAX, 0.30f);
    g.scarcityPerItemPerTickCap = sConfigMgr->GetOption<uint32_t>(CFG_SCARCITY_PER_TICK_ITEM_CAP, 1);

    g.vendorMinMarkup = sConfigMgr->GetOption<float>(CFG_VENDOR_MIN_MARKUP, 0.25f);
    g.vendorConsiderBuyPrice = sConfigMgr->GetOption<bool>(CFG_VENDOR_CONSIDER_BUYPRICE, true);
    g.vendorSoldCache.clear();

    g.stDefault = sConfigMgr->GetOption<uint32_t>(CFG_STACK_DEFAULT, 20u);
    g.stCloth = sConfigMgr->GetOption<uint32_t>(CFG_STACK_CLOTH, g.stDefault);
    g.stOre = sConfigMgr->GetOption<uint32_t>(CFG_STACK_ORE, g.stDefault);
    g.stBar = sConfigMgr->GetOption<uint32_t>(CFG_STACK_BAR, g.stDefault);
    g.stHerb = sConfigMgr->GetOption<uint32_t>(CFG_STACK_HERB, g.stDefault);
    g.stLeather = sConfigMgr->GetOption<uint32_t>(CFG_STACK_LEATHER, g.stDefault);
    g.stDust = sConfigMgr->GetOption<uint32_t>(CFG_STACK_DUST, g.stDefault);
    g.stGem = sConfigMgr->GetOption<uint32_t>(CFG_STACK_GEM, g.stDefault);
    g.stStone = sConfigMgr->GetOption<uint32_t>(CFG_STACK_STONE, g.stDefault);
    g.stMeat = sConfigMgr->GetOption<uint32_t>(CFG_STACK_MEAT, g.stDefault);
    g.stBandage = sConfigMgr->GetOption<uint32_t>(CFG_STACK_BANDAGE, g.stDefault);
    g.stPotion = sConfigMgr->GetOption<uint32_t>(CFG_STACK_POTION, 5u);
    g.stInk = sConfigMgr->GetOption<uint32_t>(CFG_STACK_INK, 10u);
    g.stPigment = sConfigMgr->GetOption<uint32_t>(CFG_STACK_PIGMENT, 20u);
    g.stFish = sConfigMgr->GetOption<uint32_t>(CFG_STACK_FISH, 20u);
    g.stacksLow = sConfigMgr->GetOption<uint32_t>(CFG_STACKS_LOW, 2u);
    g.stacksMid = sConfigMgr->GetOption<uint32_t>(CFG_STACKS_MID, 3u);
    g.stacksHigh = sConfigMgr->GetOption<uint32_t>(CFG_STACKS_HIGH, 2u);

    g.debugContextLogs = sConfigMgr->GetOption<bool>(CFG_DEBUG_CONTEXT_LOGS, false);
    g.loopEnabled = sConfigMgr->GetOption<bool>(CFG_LOOP_ENABLED, true);

    g.caps.InitDefaults();
    g.caps.enabled = sConfigMgr->GetOption<bool>(CFG_CAP_ENABLED, true);
    g.caps.totalPerCycleLimit = sConfigMgr->GetOption<uint32_t>(CFG_CAP_TOTAL, 150u);
    g.caps.perHouseLimit[0] = sConfigMgr->GetOption<uint32_t>(CFG_CAP_HOUSE_ALLI, 80u);
    g.caps.perHouseLimit[1] = sConfigMgr->GetOption<uint32_t>(CFG_CAP_HOUSE_HORDE, 80u);
    g.caps.perHouseLimit[2] = sConfigMgr->GetOption<uint32_t>(CFG_CAP_HOUSE_NEUT, 120u);

    auto FAM = [](char const *n)
    { return std::string(CFG_CAP_FAMILY_PREFIX) + n; };
    auto loadFam = [&](Family f, char const *key, uint32_t def)
    { g.caps.familyLimit[(size_t)f] = sConfigMgr->GetOption<uint32_t>(key, def); };
    loadFam(Family::Herb, FAM("Herb").c_str(), 60u);
    loadFam(Family::Ore, FAM("Ore").c_str(), 50u);
    loadFam(Family::Bar, FAM("Bar").c_str(), 50u);
    loadFam(Family::Cloth, FAM("Cloth").c_str(), 60u);
    loadFam(Family::Leather, FAM("Leather").c_str(), 40u);
    loadFam(Family::Dust, FAM("Dust").c_str(), 40u);
    loadFam(Family::Essence, FAM("Essence").c_str(), 40u);
    loadFam(Family::Shard, FAM("Shard").c_str(), 40u);
    loadFam(Family::Stone, FAM("Stone").c_str(), 40u);
    loadFam(Family::Meat, FAM("Meat").c_str(), 20u);
    loadFam(Family::Fish, FAM("Fish").c_str(), 20u);
    loadFam(Family::Gem, FAM("Gem").c_str(), 30u);
    loadFam(Family::Bandage, FAM("Bandage").c_str(), 10u);
    loadFam(Family::Potion, FAM("Potion").c_str(), 20u);
    loadFam(Family::Ink, FAM("Ink").c_str(), 20u);
    loadFam(Family::Pigment, FAM("Pigment").c_str(), 20u);
    loadFam(Family::Elemental, FAM("Elemental").c_str(), 30u);
    loadFam(Family::Other, FAM("Other").c_str(), 80u);

    g.avgGoldPerQuest = sConfigMgr->GetOption<float>(CFG_ECON_GOLD_PER_QUEST, 10.0f);
    auto ECON = [](const char *fam)
    { return std::string(CFG_ECON_QPF_PREFIX) + fam; };
    auto loadQpf = [&](Family f, char const *key, uint32_t def)
    { g.questsPerFamily[(size_t)f] = sConfigMgr->GetOption<uint32_t>(key, def); };
    loadQpf(Family::Herb, ECON("Herb").c_str(), 1u);
    loadQpf(Family::Ore, ECON("Ore").c_str(), 1u);
    loadQpf(Family::Bar, ECON("Bar").c_str(), 1u);
    loadQpf(Family::Cloth, ECON("Cloth").c_str(), 1u);
    loadQpf(Family::Leather, ECON("Leather").c_str(), 1u);
    loadQpf(Family::Dust, ECON("Dust").c_str(), 1u);
    loadQpf(Family::Essence, ECON("Essence").c_str(), 1u);
    loadQpf(Family::Shard, ECON("Shard").c_str(), 1u);
    loadQpf(Family::Stone, ECON("Stone").c_str(), 1u);
    loadQpf(Family::Meat, ECON("Meat").c_str(), 1u);
    loadQpf(Family::Fish, ECON("Fish").c_str(), 1u);
    loadQpf(Family::Gem, ECON("Gem").c_str(), 1u);
    loadQpf(Family::Bandage, ECON("Bandage").c_str(), 1u);
    loadQpf(Family::Potion, ECON("Potion").c_str(), 1u);
    loadQpf(Family::Ink, ECON("Ink").c_str(), 1u);
    loadQpf(Family::Pigment, ECON("Pigment").c_str(), 1u);
    loadQpf(Family::Other, ECON("Other").c_str(), 1u);

    BuyEngineConfig bec;
    bec.enabled = sConfigMgr->GetOption<bool>(CFG_BUY_ENABLED, false);
    uint32_t budgetGold = sConfigMgr->GetOption<uint32_t>(CFG_BUY_BUDGET_GOLD, 50u);
    bec.budgetCopper = budgetGold * 10000u;
    bec.minMargin = sConfigMgr->GetOption<float>(CFG_BUY_MIN_MARGIN, 0.15f);
    bec.perItemPerCycleCap = sConfigMgr->GetOption<uint32_t>(CFG_BUY_PER_ITEM_CAP, 2u);
    bec.maxScanRows = sConfigMgr->GetOption<uint32_t>(CFG_BUY_MAX_SCAN_ROWS, 2000u);
    bec.blockTrashAndCommon = sConfigMgr->GetOption<bool>(CFG_BUY_BLOCK_TRASH_COMMON, true);
    bec.vendorConsiderBuyPrice = g.vendorConsiderBuyPrice;
    bec.neverAboveVendorBuyPrice = g.neverBuyAboveVendorBuyPrice;
    bec.minPriceCopper = g.minPriceCopper;
    bec.scarcityEnabled = g.scarcityEnabled;
    bec.onlineCount = 0;

    buy_.SetConfig(bec);
    buy_.SetFilters(g.allowQuality, g.whiteAllow);

    g.mulDust = sConfigMgr->GetOption<float>(CFG_PRICE_MUL_DUST, 1.0f);
    g.mulEssence = sConfigMgr->GetOption<float>(CFG_PRICE_MUL_ESSENCE, 1.25f);
    g.mulShard = sConfigMgr->GetOption<float>(CFG_PRICE_MUL_SHARD, 2.0f);
    g.mulElemental = sConfigMgr->GetOption<float>(CFG_PRICE_MUL_ELEMENTAL, 3.0f);
    g.mulRareRaw = sConfigMgr->GetOption<float>(CFG_PRICE_MUL_RARERAW, 3.0f);
    DynamicAHDifficulty::Build();

    static bool catInit = false;
    if (!catInit)
    {
        DynamicAHPlanner::InitCategorySetsOnce();
        catInit = true;
    }

    g.tickPlanCounts.clear();
    g.cycle.Clear();
    g.caps.ResetCounts();
    g.nextRunMs = NowMs() + 5000;

    LOG_INFO("mod.dynamicah", "ModDynamicAH configured: seller={} every {}m; dryRun={} minPrice={}c; context={} scarcity={} cap/tick={} buy.enabled={}",
             g.enableSeller, g.intervalMin, g.dryRun, g.minPriceCopper, g.contextEnabled, g.scarcityEnabled, g.scarcityPerItemPerTickCap, bec.enabled);
}

void Service::CmdFund(ChatHandler *handler, uint32 gold, std::string const &which)
{
    // Convert gold to copper
    uint64 copper = uint64(gold) * 10000ull;

    // Normalize selector
    std::string w = which;
    for (char &ch : w)
        ch = std::tolower(ch);

    std::vector<uint32> guids;
    auto add = [&](uint32 g)
    { if (g) guids.push_back(g); };

    if (w == "all")
    {
        add(state_.ownerAlliance);
        add(state_.ownerHorde);
        add(state_.ownerNeutral);
    }
    else if (w == "a" || w == "ally" || w == "alliance")
        add(state_.ownerAlliance);
    else if (w == "h" || w == "horde")
        add(state_.ownerHorde);
    else if (w == "n" || w == "neutral")
        add(state_.ownerNeutral);
    else
    {
        if (handler)
            handler->PSendSysMessage("动态AH: 未知目标 %s(使用: all|a|h|n)", which.c_str());
        return;
    }

    if (guids.empty())
    {
        if (handler)
            handler->PSendSysMessage("ModDynamicAH: no seller GUIDs configured. Run `.dah setup` first.");
        return;
    }

    // Build one UPDATE for all selected GUIDs
    std::string inlist;
    for (size_t i = 0; i < guids.size(); ++i)
    {
        if (i)
            inlist += ",";
        inlist += std::to_string(guids[i]);
    }

    std::string sql =
        "UPDATE characters SET money = money + " + std::to_string(copper) +
        " WHERE guid IN (" + inlist + ")";
    CharacterDatabase.DirectExecute(sql.c_str());

    if (handler)
        handler->PSendSysMessage("动态AH: 已注入 {} 金币到 [{}](GUID: {})",
                                 gold, which.c_str(), inlist.c_str());
}

void Service::CmdCapsShow(ChatHandler *handler)
{
    auto const &c = state_.caps;
    if (!handler)
        return;
    handler->PSendSysMessage("上限: 启用={} 每周期总限额={}", c.enabled ? "ON" : "OFF", c.totalPerCycleLimit);
    handler->PSendSysMessage("per-house: A={} H={} N={} (planned: A={} H={} N={})",
                             c.perHouseLimit[0], c.perHouseLimit[1], c.perHouseLimit[2],
                             c.perHousePlanned[0], c.perHousePlanned[1], c.perHousePlanned[2]);
    std::string fam = "family limits:";
    for (size_t i = 0; i < (size_t)Family::COUNT; ++i)
    {
        fam += fmt::format(" {}={}", i, c.familyLimit[i]);
        if ((i + 1) % 6 == 0)
        {
            handler->PSendSysMessage("{}", fam.c_str());
            fam = "  ";
        }
    }
    if (fam.size() > 2)
        handler->PSendSysMessage("{}", fam.c_str());
}

void Service::DoOneCycle()
{
    auto &g = state_;

    g.tickPlanCounts.clear();
    g.cycle.Clear();
    g.caps.ResetCounts();

    planner_.BuildScarcityCache(g);
    planner_.BuildContextPlan(ToPlannerCfg(g));
    planner_.BuildRandomPlan(ToPlannerCfg(g));
    // Make sure the plan posts to the AH
    // state_.postQueue.Clear(); <- to avoid posting to the AH
    {
        auto allPosts = planner_.Queue().Drain(UINT32_MAX);
        for (auto const &r : allPosts)
            state_.postQueue.Push(r);
    }
    buy_.ResetCycle();
    buy_.SetFilters(g.allowQuality, g.whiteAllow);
    buy_.SetOwners(g.ownerAlliance, g.ownerHorde, g.ownerNeutral);

    auto fairFn = [&](uint32_t itemId, uint32_t active) -> PricingResult
    {
        auto *tmpl = sObjectMgr->GetItemTemplate(itemId);
        PricingInputs pin{tmpl, active, g.cycle.onlineCount, g.minPriceCopper};
        return DynamicAHPricing::Compute(pin);
    };
    auto scarceFn = [&](uint32_t itemId, AuctionHouseId house) -> uint32_t
    { return planner_.ScarcityCount(itemId, house); };
    auto vendorFn = [&](uint32_t itemId) -> std::pair<bool, uint32_t>
    {
        auto *tmpl = sObjectMgr->GetItemTemplate(itemId);
        bool isVendor = (tmpl && tmpl->BuyPrice > 0);
        uint32_t buy = (tmpl ? tmpl->BuyPrice : 0);
        return {isVendor, buy};
    };

    buy_.BuildPlan(scarceFn, fairFn, vendorFn);
}

void Service::OnUpdate(uint32_t /*diff*/)
{
    auto &g = state_;
    if (!g.loopEnabled)
        return;

    uint64_t now = (uint64_t)GameTime::GetGameTimeMS().count();
    if (now >= g.nextRunMs)
    {
        DoOneCycle();
        g.nextRunMs = now + (uint64_t)g.intervalMin * MINUTE * IN_MILLISECONDS;
    }

    buy_.SetOwners(g.ownerAlliance, g.ownerHorde, g.ownerNeutral);
    ModDynamicAH::DynamicAHPosting::ApplyPlanOnWorld(g, 10, nullptr);
    buy_.Apply(10, /*dry*/ g.dryRun, /*handler*/ nullptr);
}

void Service::PlanOnce(ChatHandler *handler)
{
    auto &g = state_;
    g.tickPlanCounts.clear();
    g.cycle.Clear();
    g.caps.ResetCounts();
    planner_.BuildScarcityCache(g);
    planner_.BuildContextPlan(ToPlannerCfg(g));
    planner_.BuildRandomPlan(ToPlannerCfg(g));
    // Make sure the plan posts to the AH
    // state_.postQueue.Clear(); <- to avoid posting to the AH
    {
        auto batch = planner_.Queue().Drain(std::numeric_limits<uint32_t>::max());
        for (auto &r : batch)
            state_.postQueue.Push(std::move(r));
    }
    buy_.ResetCycle();
    buy_.SetFilters(g.allowQuality, g.whiteAllow);
    buy_.SetOwners(g.ownerAlliance, g.ownerHorde, g.ownerNeutral);

    auto fairFn = [&](uint32_t itemId, uint32_t active) -> PricingResult
    {
        auto *tmpl = sObjectMgr->GetItemTemplate(itemId);
        PricingInputs pin{tmpl, active, g.cycle.onlineCount, g.minPriceCopper};
        return DynamicAHPricing::Compute(pin);
    };
    auto scarceFn = [&](uint32_t itemId, AuctionHouseId house) -> uint32_t
    { return planner_.ScarcityCount(itemId, house); };
    auto vendorFn = [&](uint32_t itemId) -> std::pair<bool, uint32_t>
    {
        auto *tmpl = sObjectMgr->GetItemTemplate(itemId);
        bool isVendor = (tmpl && tmpl->BuyPrice > 0);
        uint32_t buy = (tmpl ? tmpl->BuyPrice : 0);
        return {isVendor, buy};
    };
    buy_.BuildPlan(scarceFn, fairFn, vendorFn);

    handler->PSendSysMessage("动态AH: 计划已构建。上架: {} 购买: {}",
                             state_.postQueue.Size(), buy_.QueueSize());
}

void Service::ApplyOnce(ChatHandler *handler)
{
    uint32_t beforeP = state_.postQueue.Size();
    size_t beforeB = buy_.QueueSize();

    buy_.SetOwners(state_.ownerAlliance, state_.ownerHorde, state_.ownerNeutral);
    ModDynamicAH::DynamicAHPosting::ApplyPlanOnWorld(state_, 100, handler);
    buy_.Apply(100, state_.dryRun, handler);

    uint32_t posted = (beforeP > state_.postQueue.Size()) ? (beforeP - state_.postQueue.Size()) : 0u;
    uint32_t buysApplied = (beforeB > buy_.QueueSize()) ? (uint32_t)(beforeB - buy_.QueueSize()) : 0u;

    handler->PSendSysMessage("动态AH: 已应用({})。已上架={} 剩余={} | 已购买={} 剩余={}",
                             state_.dryRun ? "dry-run" : "live",
                             posted, state_.postQueue.Size(), buysApplied, buy_.QueueSize());
}

void Service::ClearQueues(ChatHandler *handler)
{
    ModDynamicAH::DynamicAHPosting::ApplyPlanOnWorld(state_, 1000000, handler);
    buy_.Apply(1000000, state_.dryRun, handler);
    handler->PSendSysMessage("动态AH: 已清除待处理(模拟={}) 上架={} 购买={}",
                             state_.dryRun ? 1 : 0, state_.postQueue.Size(), buy_.QueueSize());
}

void Service::ShowQueue(ChatHandler *handler)
{
    handler->PSendSysMessage("动态AH: 上架队列={} 购买队列={} 预算已用={}/{}",
                             state_.postQueue.Size(), buy_.QueueSize(), buy_.BudgetUsed(), buy_.BudgetLimit());
}

void Service::ToggleLoop(bool enable, ChatHandler *handler)
{
    state_.loopEnabled = enable;
    if (enable)
        state_.nextRunMs = (uint64_t)GameTime::GetGameTimeMS().count() + 1000;
    handler->PSendSysMessage("动态AH: 循环已{}", enable ? "enabled" : "disabled");
}

void Service::SetDryRun(bool dry, ChatHandler *handler)
{
    state_.dryRun = dry;
    handler->PSendSysMessage("动态AH: 模拟模式 = {}", dry ? 1u : 0u);
}

void Service::SetInterval(uint32_t minutes, ChatHandler *handler)
{
    state_.intervalMin = minutes ? minutes : 1;
    state_.nextRunMs = (uint64_t)GameTime::GetGameTimeMS().count() + 1000;
    handler->PSendSysMessage("动态AH: 间隔 = {}分钟", state_.intervalMin);
}

void Service::ShowStatus(ChatHandler *handler)
{
    handler->PSendSysMessage(
        "ModDynamicAH: seller={} dryRun={} interval={}m owners A/H/N={}/{}/{} caps={} totalCap={} context={} postQ={} buyQ={}",
        state_.enableSeller ? 1u : 0u,
        state_.dryRun ? 1u : 0u,
        state_.intervalMin,
        state_.ownerAlliance, state_.ownerHorde, state_.ownerNeutral,
        state_.caps.enabled ? 1u : 0u, state_.caps.totalPerCycleLimit,
        state_.contextEnabled ? 1u : 0u,
        state_.postQueue.Size(), buy_.QueueSize());
}

void Service::CmdCapsEnable(ChatHandler *handler, bool on)
{
    state_.caps.enabled = on;
    if (handler)
        handler->PSendSysMessage("上限: 已{}", on ? "ON" : "OFF");
    LOG_INFO("mod_dynamic_ah", "上限: 已{}", on ? "ON" : "OFF");
}

void Service::CmdCapsReset(ChatHandler *handler)
{
    state_.caps.ResetCounts();
    if (handler)
        handler->PSendSysMessage("上限: 计数已重置");
    LOG_INFO("mod_dynamic_ah", "上限: 计数已重置");
}

void Service::CmdCapsDefaults(ChatHandler *handler)
{
    state_.caps.InitDefaults();
    if (handler)
        handler->PSendSysMessage("上限: 默认值已恢复");
    LOG_INFO("mod_dynamic_ah", "上限: 默认值已恢复");
}

void Service::CmdCapsSetTotal(ChatHandler *handler, uint32 value)
{
    state_.caps.totalPerCycleLimit = value;
    if (handler)
        handler->PSendSysMessage("上限: 每周期总限额={}", value);
    LOG_INFO("mod_dynamic_ah", "caps: set totalPerCycleLimit={}", value);
}

void Service::CmdCapsSetHouse(ChatHandler *handler, std::string which, uint32 value)
{
    int idx = HouseIndexByName(which);
    if (idx < 0 || idx > 2)
    {
        if (handler)
            handler->PSendSysMessage("上限: 未知拍卖行 {}，使用 A|H|N", which.c_str());
        return;
    }
    state_.caps.perHouseLimit[idx] = value;
    if (handler)
        handler->PSendSysMessage("caps: house[{}]={}", which.c_str(), value);
    LOG_INFO("mod_dynamic_ah", "caps: set house[{}] = {}", which, value);
}

void Service::CmdCapsSetFamily(ChatHandler *handler, std::string famName, uint32 value)
{
    ModDynamicAH::Family fam;
    if (!ParseFamilyName(famName, fam))
    {
        if (handler)
            handler->PSendSysMessage("上限: 未知类别 {}", famName.c_str());
        return;
    }
    state_.caps.familyLimit[(size_t)fam] = value;
    if (handler)
        handler->PSendSysMessage("caps: family[{}]={}", famName.c_str(), value);
    LOG_INFO("mod_dynamic_ah", "caps: set family[{}] = {}", famName, value);
}

bool Service::CmdContext(ChatHandler *handler, Optional<std::string> keyOpt, Optional<uint32> valOpt)
{
    // Show current context
    if (!keyOpt)
    {
        handler->PSendSysMessage("上下文: 启用={} 每区间上限={} 权重加成={:.2f} 跳过商贩={} 调试={}",
                                 state_.contextEnabled ? "ON" : "OFF",
                                 state_.contextMaxPerBracket,
                                 double(state_.contextWeightBoost),
                                 state_.contextSkipVendor ? "ON" : "OFF",
                                 state_.debugContextLogs ? "ON" : "OFF");
        return true;
    }

    std::string key = *keyOpt;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);

    auto needVal = [&](char const *what) -> bool
    {
        if (valOpt)
            return true;
        handler->PSendSysMessage("context: '{}' requires a value", what);
        return false;
    };

    if (key == "maxperbracket")
    {
        if (!needVal("maxperbracket"))
            return false;
        state_.contextMaxPerBracket = *valOpt;
        handler->PSendSysMessage("context: maxPerBracket={}", state_.contextMaxPerBracket);
        LOG_INFO("mod_dynamic_ah", "context: set maxPerBracket={}", state_.contextMaxPerBracket);
        return true;
    }
    else if (key == "weightboost")
    {
        if (!needVal("weightboost%%"))
            return false;
        double pct = double(*valOpt);
        if (pct < 0.0)
            pct = 0.0;
        state_.contextWeightBoost = float(pct / 100.0);
        handler->PSendSysMessage("context: weightBoost={:.2f} (from {}%%)", double(state_.contextWeightBoost), uint32(pct));
        LOG_INFO("mod_dynamic_ah", "context: set weightBoost={:.2f}", double(state_.contextWeightBoost));
        return true;
    }
    else if (key == "skipvendor")
    {
        if (!needVal("skipvendor 0|1"))
            return false;
        state_.contextSkipVendor = (*valOpt != 0);
        handler->PSendSysMessage("context: skipVendor={}", state_.contextSkipVendor ? "ON" : "OFF");
        LOG_INFO("mod_dynamic_ah", "context: set skipVendor={}", state_.contextSkipVendor ? "ON" : "OFF");
        return true;
    }
    else if (key == "debug")
    {
        if (!needVal("debug 0|1"))
            return false;
        state_.debugContextLogs = (*valOpt != 0);
        handler->PSendSysMessage("context: debug={}", state_.debugContextLogs ? "ON" : "OFF");
        LOG_INFO("mod_dynamic_ah", "context: set debug={}", state_.debugContextLogs ? "ON" : "OFF");
        return true;
    }
    else if (key == "enable")
    {
        if (!needVal("enable 0|1"))
            return false;
        state_.contextEnabled = (*valOpt != 0);
        handler->PSendSysMessage("context: enabled={}", state_.contextEnabled ? "ON" : "OFF");
        LOG_INFO("mod_dynamic_ah", "context: set enabled={}", state_.contextEnabled ? "ON" : "OFF");
        return true;
    }

    handler->PSendSysMessage("context: unknown key '{}'", key.c_str());
    return false;
}
