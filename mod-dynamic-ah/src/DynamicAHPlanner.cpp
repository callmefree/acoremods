#include "DynamicAHPlanner.h"
#include "DynamicAHSelection.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "GameTime.h"
#include "World.h"
#include "Player.h"
#include "SharedDefines.h"
#include <algorithm>
#include <vector>
#include <unordered_set>
#include <set>
#include "DynamicAHRecipes.h"

namespace ModDynamicAH
{

    static std::unordered_set<uint32> gEss, gShr, gEle, gRare;
    static bool gCatInit = false;

    // Per-unit price anchors (copper) for keystone mats whose live value is driven by demand, not
    // by recipe skill level. The heuristic pricing underestimates these badly, so we override it.
    // Values are deliberately approximate and server-dependent — tune to your economy.
    static uint32 DemandAnchorCopper(uint32 itemId)
    {
        switch (itemId)
        {
        case 36908: return 250000; // Frost Lotus
        case 36910: return 180000; // Titanium Ore
        case 36912: return 8000;   // Saronite Ore (oversupplied -> intentionally cheap)
        case 36860: return 150000; // Eternal Fire
        case 35625: return 130000; // Eternal Life
        case 35622: return 90000;  // Eternal Water
        case 35623: return 70000;  // Eternal Air
        case 35627: return 45000;  // Eternal Shadow
        case 35624: return 35000;  // Eternal Earth
        case 34054: return 15000;  // Infinite Dust
        case 34055: return 50000;  // Greater Cosmic Essence
        case 34057: return 120000; // Abyss Crystal
        case 34052: return 90000;  // Dream Shard
        case 36919: return 800000; // Cardinal Ruby
        case 36922: return 600000; // King's Amber
        case 36931: return 500000; // Ametrine
        case 36925: return 450000; // Majestic Zircon
        case 36928: return 400000; // Dreadstone
        case 36934: return 350000; // Eye of Zul
        case 36918: return 150000; // Scarlet Ruby
        default: return 0;
        }
    }

    std::unordered_set<uint32> &DynamicAHPlanner::EssenceSet() { return gEss; }
    std::unordered_set<uint32> &DynamicAHPlanner::ShardSet() { return gShr; }
    std::unordered_set<uint32> &DynamicAHPlanner::ElementalSet() { return gEle; }
    std::unordered_set<uint32> &DynamicAHPlanner::RareRawSet() { return gRare; }

    void DynamicAHPlanner::InitCategorySetsOnce()
    {
        if (gCatInit)
            return;
        auto addList = [](auto const &tbl, std::unordered_set<uint32> &tgt)
        {
            for (auto const &br : tbl)
                for (uint32 id : br.items)
                    tgt.insert(id);
        };
        addList(ENCH_ESSENCE, EssenceSet());
        addList(ENCH_SHARDS, ShardSet());
        addList(ELEMENTALS, ElementalSet());
        addList(RARE_RAW, RareRawSet());
        gCatInit = true;
    }

    double DynamicAHPlanner::CategoryMul(PlannerConfig const &cfg, uint32 itemId)
    {
        if (EssenceSet().count(itemId))
            return cfg.mulEssence;
        if (ShardSet().count(itemId))
            return cfg.mulShard;
        if (ElementalSet().count(itemId))
            return cfg.mulElemental;
        if (RareRawSet().count(itemId))
            return cfg.mulRareRaw;
        return 1.0;
    }

    void DynamicAHPlanner::ResetTick(uint32 onlineCount)
    {
        _queue.Clear();
        _perTickPlanCap.clear();
        _scarcity.Clear();
        _scarcity.Rebuild();
        _online = onlineCount;
        InitCategorySetsOnce();
    }

    static inline uint8 HouseIndex(AuctionHouseId h)
    {
        switch (h)
        {
        case AuctionHouseId::Alliance:
            return 0;
        case AuctionHouseId::Horde:
            return 1;
        default:
            return 2;
        }
    }

    double DynamicAHPlanner::Jitter(uint32 itemId)
    {
        uint32 t = static_cast<uint32>(GameTime::GetGameTime().count());
        uint32 seed = t ^ (itemId * 2654435761u);
        int delta = int(seed % 11) - 5; // -5..+5
        return 1.0 + double(delta) / 100.0;
    }

    uint32 DynamicAHPlanner::ClampToStackable(ItemTemplate const *tmpl, uint32 desired)
    {
        uint32 maxStack = (tmpl && tmpl->Stackable > 0) ? tmpl->Stackable : 1u;
        return std::min(desired ? desired : 1u, maxStack);
    }

    uint32 DynamicAHPlanner::ScarcityCount(uint32 itemId, AuctionHouseId house) const
    {
        return _scarcity.Count(itemId, house);
    }

    void DynamicAHPlanner::ResetCycleCaps(PlannerConfig const &cfg)
    {
        _capsEnabled = cfg.capsEnabled;
        _capTotal = cfg.capTotalPerCycle;
        for (int i = 0; i < 3; ++i)
            _capHouse[i] = cfg.capPerHouse[i];
        for (size_t i = 0; i < (size_t)Family::COUNT; ++i)
            _capFamily[i] = cfg.capPerFamily[i];

        _plTotal = 0;
        _plHouse[0] = _plHouse[1] = _plHouse[2] = 0;
        for (size_t i = 0; i < (size_t)Family::COUNT; ++i)
            _plFamily[i] = 0;
        _perTickPlanCap.clear();
    }

    bool DynamicAHPlanner::TryPlanOnce(AuctionHouseId house, Family fam)
    {
        uint8 h = HouseIndex(house);
        size_t f = (size_t)fam;
        if (_capsEnabled)
        {
            if (_capTotal && _plTotal >= _capTotal)
                return false;
            if (_capHouse[h] && _plHouse[h] >= _capHouse[h])
                return false;
            if (_capFamily[f] && _plFamily[f] >= _capFamily[f])
                return false;
        }
        ++_plTotal;
        ++_plHouse[h];
        ++_plFamily[f];
        return true;
    }

    void DynamicAHPlanner::PriceWithPolicies(PlannerConfig const &cfg, Family fam, uint32 itemId,
                                             ItemTemplate const *tmpl, AuctionHouseId house,
                                             uint32 &outStart, uint32 &outBuy) const
    {
        uint32 active = cfg.scarcityEnabled ? ScarcityCount(itemId, house) : 0;

        PricingInputs in;
        in.tmpl = tmpl;
        in.activeInHouse = active;
        in.onlineCount = _scarcity.OnlineCount();
        in.minPriceCopper = cfg.minPriceCopper; // may be overridden for stackable mats below

        // ---- Recipe-driven bounds (per STACK) ----
        ModDynamicAH::RecipeUsageIndex::Instance().EnsureBuilt();
        uint16 req = ModDynamicAH::RecipeUsageIndex::Instance().EffectiveSkillForReagent(itemId);
        uint16 maxReq = ModDynamicAH::RecipeUsageIndex::Instance().MaxSkillForReagent(itemId);
        if (req > 450)
            req = 450;
        if (maxReq > 450)
            maxReq = 450;

        auto lerp = [](double a, double b, double t)
        { return a + (b - a) * std::clamp(t, 0.0, 1.0); };

        auto stackBaseFloorG = [req, lerp]() -> double
        {
            // Floors per stack tuned for low tiers:
            // 0..75: 0.4..0.8g, 75..150: 0.8..1.6g, 150..225: 1.6..4g,
            // 225..300: 4..12g, 300..375: 12..30g, 375..450: 30..60g
            if (req <= 75)
                return lerp(0.4, 0.8, double(req) / 75.0);
            if (req <= 150)
                return lerp(0.8, 1.6, double(req - 75) / 75.0);
            if (req <= 225)
                return lerp(1.6, 4.0, double(req - 150) / 75.0);
            if (req <= 300)
                return lerp(4.0, 12.0, double(req - 225) / 75.0);
            if (req <= 375)
                return lerp(12.0, 30.0, double(req - 300) / 75.0);
            return lerp(30.0, 60.0, double(req - 375) / 75.0);
        };

        // modest premium if many high-tier recipes also use it
        double spread = std::max(0, int(maxReq) - int(req));        // 0..450
        double boost = std::clamp(spread / 200.0, 0.0, 1.0) * 0.20; // up to +20%

        uint32 stackSize = std::max<uint32>(1u, tmpl->Stackable);
        bool isStackableMat =
            (stackSize > 1) && (fam != Family::Other); // treat mats (cloth/ore/herb/etc.) specially

        uint32 recipeUnitFloor = 0;
        uint32 recipeUnitCeil = 0;

        if (req > 0)
        {
            double floorG = stackBaseFloorG() * (1.0 + boost);
            // ceilings: allow more headroom at higher tiers
            double spanMul =
                (req <= 150) ? 2.0 : (req <= 300) ? 2.5
                                                  : 3.0;
            double ceilG = floorG * spanMul;

            recipeUnitFloor = uint32(std::lround(floorG * 10000.0 / double(stackSize)));
            recipeUnitCeil = uint32(std::lround(ceilG * 10000.0 / double(stackSize)));

            // For stackable mats, use recipe floor as the effective min so low tiers stay cheap.
            if (isStackableMat)
                in.minPriceCopper = std::max<uint32>(recipeUnitFloor, 100u); // at least 1s
            else
                in.minPriceCopper = std::max<uint32>(in.minPriceCopper, recipeUnitFloor);
        }

        // ---- Base unit price from engine ----
        PricingResult base = DynamicAHPricing::Compute(in); // unit-level
        uint32 unitStart = base.startBid;
        uint32 unitBuy = std::max<uint32>(base.buyout, unitStart + 1);

        // ---- Scarcity/category/jitter (unit) ----
        auto mulRound = [](uint32 v, double f) -> uint32
        { return uint32(std::lround(double(v) * f)); };
        double scarcityBoost = cfg.scarcityEnabled ? (1.0 + cfg.scarcityPriceBoostMax / double(1 + active)) : 1.0;
        double catMul = CategoryMul(cfg, itemId);
        double jitter = Jitter(itemId);

        unitStart = mulRound(unitStart, scarcityBoost * catMul * jitter);
        unitBuy = mulRound(unitBuy, scarcityBoost * catMul * jitter);
        if (unitBuy <= unitStart)
            unitBuy = unitStart + 1;

        // ---- Friendly rounding (unit): >=1g → 5s steps, else 1s ----
        auto roundTo = [](uint32 coppers, uint32 stepC) -> uint32
        {
            uint32 r = coppers % stepC;
            uint32 down = coppers - r;
            uint32 up = down + stepC;
            return (coppers - down < up - coppers) ? down : up;
        };
        uint32 step = (unitBuy >= 10000u) ? 500u : 100u;
        unitBuy = roundTo(unitBuy, step);
        unitStart = std::min<uint32>((unitBuy > 0 ? unitBuy - 1 : 0), roundTo(unitStart, step));

        // ---- Vendor/min floors & markup (unit) ----
        DynamicAHVendor::ApplyVendorFloor(tmpl, unitStart, unitBuy, cfg.minPriceCopper, cfg.vendorMinMarkup);

        // ---- Clamp to recipe-based ceiling for mats (prevents low tiers from ballooning) ----
        if (isStackableMat && recipeUnitCeil > 0)
        {
            if (unitBuy > recipeUnitCeil)
                unitBuy = recipeUnitCeil;
            if (unitStart >= unitBuy)
                unitStart = (unitBuy > 0 ? unitBuy - 1 : 0);
        }

        // ---- Demand anchor: override the heuristic for keystone, demand-driven mats ----
        if (uint32 anchor = DemandAnchorCopper(itemId))
        {
            unitBuy = mulRound(anchor, jitter);
            unitStart = uint32(double(unitBuy) * 0.92);
            if (unitBuy <= unitStart)
                unitBuy = unitStart + 1;
        }

        outStart = unitStart;
        outBuy = unitBuy;
    }

    void DynamicAHPlanner::BuildRandomPlan(PlannerConfig const &cfg)
    {
        if (!cfg.enableSeller)
            return;

        SelectionConfig sel;
        sel.blockTrashAndCommon = cfg.blockTrashAndCommon;
        for (int i = 0; i < 6; ++i)
            sel.allowQuality[i] = cfg.allowQuality[i];
        sel.whitelist = cfg.whitelist;
        sel.maxRandomPostsPerCycle = cfg.maxRandomPerCycle;
        sel.minPriceCopper = cfg.minPriceCopper;

        auto candidates = DynamicAHSelection::PickRandomSellables(sel, cfg.maxRandomPerCycle);
        for (auto const &c : candidates)
        {
            ItemTemplate const *tmpl = c.tmpl;
            if (!tmpl)
                continue;

            // simple house distribution
            uint8 which = static_cast<uint8>(c.itemId % 3);
            AuctionHouseId house = (which == 0) ? AuctionHouseId::Alliance : (which == 1) ? AuctionHouseId::Horde
                                                                                          : AuctionHouseId::Neutral;

            if (!TryPlanOnce(house, Family::Other))
                continue;

            uint32 startBid = 0, buyout = 0;
            PriceWithPolicies(cfg, Family::Other, c.itemId, tmpl, house, startBid, buyout);

            uint32 count = ClampToStackable(tmpl, cfg.stDefault);
            _queue.Push(PostRequest{house, c.itemId, count, startBid, buyout, 24 * HOUR});
        }
    }

    // ---- Context planner (short, but uses your ProfessionMats.h tables) ----
    // Helpers to pick items for a skill bracket:
    template <size_t N>
    static MatBracket const *FindBracket(uint16 s, std::array<MatBracket, N> const &tab)
    {
        MatBracket const *last = nullptr;
        for (MatBracket const &b : tab)
        {
            if (s >= b.minSkill && s < b.maxSkill)
                return &b;
            last = &b; // tables are ordered ascending, so this ends as the highest bracket
        }
        // A maxed skill (e.g. 450) sits exactly on the top bracket's exclusive upper bound; map it
        // to the highest bracket instead of returning nothing.
        if (last && s >= last->maxSkill)
            return last;
        return nullptr;
    }

    // Enqueue for a specific auction house without needing a Player*
    static bool EnqueueHouse(AuctionHouseId house, PlannerConfig const &cfg, DynamicAHPlanner *self,
                             Family fam, uint32 itemId, uint32 desiredStack, uint32 stacksToPost)
    {
        if (!itemId)
            return false;

        ItemTemplate const *tmpl = sObjectMgr->GetItemTemplate(itemId);
        if (!tmpl)
            return false;

        // Safety net: never list non-tradeable items (BoP, quest, account-bound, etc.).
        if (!IsAuctionableItem(tmpl))
            return false;

        uint32 unitStart = 0, unitBuy = 0;
        self->PriceWithPolicies(cfg, fam, itemId, tmpl, house, unitStart, unitBuy);

        uint32 count = DynamicAHPlanner::ClampToStackable(tmpl, desiredStack);
        if (count == 0)
            count = 1;

        uint64 sb = uint64(unitStart) * count;
        uint64 bo = uint64(unitBuy) * count;
        if (bo <= sb)
            bo = sb + 1;

        uint32 stackStart = sb > UINT32_MAX ? UINT32_MAX : uint32(sb);
        uint32 stackBuy = bo > UINT32_MAX ? UINT32_MAX : uint32(bo);

        char const *houseTag = (house == AuctionHouseId::Alliance ? "A" : house == AuctionHouseId::Horde ? "H"
                                                                                                         : "N");
        LOG_INFO("mod.dynamicah",
                 "plan: item={} '{}' house={} stack={} unitStart={}c unitBuy={}c stackStart={}c stackBuy={}c",
                 itemId, tmpl->Name1, houseTag, count, unitStart, unitBuy, stackStart, stackBuy);

        for (uint32 i = 0; i < stacksToPost; ++i)
        {
            if (!self->TryPlanOnce(house, fam))
                break;
            self->Queue().Push(PostRequest{house, itemId, count, stackStart, stackBuy, 24 * HOUR});
        }
        return true;
    }

    void DynamicAHPlanner::BuildContextPlan(PlannerConfig const &cfg)
    {
        // Reset per-cycle cap accounting before any enqueue (also covers BuildRandomPlan, which
        // shares the same counters and runs immediately after this).
        ResetCycleCaps(cfg);

        if (!cfg.contextEnabled)
            return;

        const uint32 stacks = cfg.stacksMid;

        // For each profession's material table, find which skill brackets the online players
        // actually sit in (per faction) and list only those tiers. De-duping by bracket means a
        // crowded server lists each needed tier once per faction house rather than once per player,
        // so listings track the population's real level progression without flooding the AH.
        auto processTable = [&](uint32 skillId, Family fam, auto const &tab, uint32 stack) -> bool
        {
            std::set<MatBracket const *> needAlliance, needHorde;
            for (auto const &kv : ObjectAccessor::GetPlayers())
            {
                Player *p = kv.second;
                if (!p || !p->IsInWorld())
                    continue;
                uint16 sv = p->GetSkillValue(skillId);
                if (!sv)
                    continue;
                if (MatBracket const *b = FindBracket(sv, tab))
                {
                    if (p->GetTeamId() == TEAM_ALLIANCE)
                        needAlliance.insert(b);
                    else
                        needHorde.insert(b);
                }
            }

            bool any = false;
            for (MatBracket const *b : needAlliance)
                for (uint32 id : b->items)
                    any |= EnqueueHouse(AuctionHouseId::Alliance, cfg, this, fam, id, stack, stacks);
            for (MatBracket const *b : needHorde)
                for (uint32 id : b->items)
                    any |= EnqueueHouse(AuctionHouseId::Horde, cfg, this, fam, id, stack, stacks);
            return any;
        };

        bool any = false;
        // Cloth (tailoring + first aid)
        any |= processTable(SKILL_TAILORING, Family::Cloth, TAILORING_CLOTH, cfg.stCloth);
        any |= processTable(SKILL_FIRST_AID, Family::Cloth, TAILORING_CLOTH, cfg.stCloth);
        // Herbs (alchemy + herbalism)
        any |= processTable(SKILL_ALCHEMY, Family::Herb, HERBS, cfg.stHerb);
        any |= processTable(SKILL_HERBALISM, Family::Herb, HERBS, cfg.stHerb);
        // Inscription (pigments + inks)
        any |= processTable(SKILL_INSCRIPTION, Family::Pigment, INSCRIPTION_PIGMENT, cfg.stPigment);
        any |= processTable(SKILL_INSCRIPTION, Family::Ink, INSCRIPTION_INK, cfg.stInk);
        // Mining / smithing
        any |= processTable(SKILL_MINING, Family::Ore, MINING_ORE, cfg.stOre);
        any |= processTable(SKILL_MINING, Family::Stone, MINING_STONE, cfg.stStone);
        any |= processTable(SKILL_BLACKSMITHING, Family::Bar, BS_BARS, cfg.stBar);
        // Leather (leatherworking + skinning)
        any |= processTable(SKILL_LEATHERWORKING, Family::Leather, LEATHERS, cfg.stLeather);
        any |= processTable(SKILL_SKINNING, Family::Leather, LEATHERS, cfg.stLeather);
        // Enchanting
        any |= processTable(SKILL_ENCHANTING, Family::Dust, ENCH_DUSTS, cfg.stDust);
        any |= processTable(SKILL_ENCHANTING, Family::Essence, ENCH_ESSENCE, cfg.stDust);
        any |= processTable(SKILL_ENCHANTING, Family::Shard, ENCH_SHARDS, 1u); // shards/crystals sell singly
        // Jewelcrafting — gems sell as singles, not stacks
        any |= processTable(SKILL_JEWELCRAFTING, Family::Gem, JEWELCRAFT_GEMS, 1u);
        // Alchemy reagents (primals / crystallized / eternals)
        any |= processTable(SKILL_ALCHEMY, Family::Elemental, ELEMENTALS, cfg.stDefault);
        // Cooking / fishing
        any |= processTable(SKILL_COOKING, Family::Meat, COOKING_MEAT, cfg.stMeat);
        any |= processTable(SKILL_FISHING, Family::Fish, FISHING_RAW, cfg.stFish);

        // Fallback: no professioned players online — keep the AH from going empty by listing a
        // light baseline of the core gathering families to both factions (still cap-bounded).
        if (!any)
        {
            auto dumpAll = [&](Family fam, auto const &tab, uint32 stack)
            {
                for (auto const &b : tab)
                    for (uint32 id : b.items)
                    {
                        EnqueueHouse(AuctionHouseId::Alliance, cfg, this, fam, id, stack, cfg.stacksLow);
                        EnqueueHouse(AuctionHouseId::Horde, cfg, this, fam, id, stack, cfg.stacksLow);
                    }
            };
            dumpAll(Family::Cloth, TAILORING_CLOTH, cfg.stCloth);
            dumpAll(Family::Herb, HERBS, cfg.stHerb);
            dumpAll(Family::Ore, MINING_ORE, cfg.stOre);
            dumpAll(Family::Bar, BS_BARS, cfg.stBar);
            dumpAll(Family::Leather, LEATHERS, cfg.stLeather);
            dumpAll(Family::Dust, ENCH_DUSTS, cfg.stDust);
            dumpAll(Family::Gem, JEWELCRAFT_GEMS, 1u);
        }
    }

    void DynamicAHPlanner::BuildScarcityCache(ModuleState const & /*s*/)
    {
        _scarcity.Clear();
        _scarcity.Rebuild();
    }
}