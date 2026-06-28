#pragma once

#include "Common.h"
#include "Optional.h"
#include "SharedDefines.h"
#include "ItemTemplate.h"
#include "AuctionHouseMgr.h"

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace ModDynamicAH
{

    // --- Families (crafting categories) ---
    enum class Family : uint8
    {
        Herb,
        Ore,
        Bar,
        Cloth,
        Leather,
        Jewelcrafting,
        Dust,
        Essence,
        Shard,
        Elemental,
        Stone,
        Meat,
        Fish,
        Gem,
        Bandage,
        Potion,
        Ink,
        Pigment,
        Other,
        COUNT
    };

    inline char const *FamilyName(Family f)
    {
        switch (f)
        {
        case Family::Herb:
            return "herb";
        case Family::Ore:
            return "ore";
        case Family::Bar:
            return "bar";
        case Family::Cloth:
            return "cloth";
        case Family::Leather:
            return "leather";
        case Family::Dust:
            return "dust";
        case Family::Essence:
            return "essence";
        case Family::Shard:
            return "shard";
        case Family::Elemental:
            return "elemental";
        case Family::Stone:
            return "stone";
        case Family::Meat:
            return "meat";
        case Family::Fish:
            return "fish";
        case Family::Gem:
            return "gem";
        case Family::Bandage:
            return "bandage";
        case Family::Potion:
            return "potion";
        case Family::Ink:
            return "ink";
        case Family::Pigment:
            return "pigment";
        default:
            return "other";
        }
    }

    // Returns true only for items the seller bot may legitimately auction, i.e. items a normal
    // player could list on the AH. Excludes Bind-on-Pickup and quest items (soulbound / "cannot
    // be dropped"), account-bound items, conjured items (they vanish), pickup-locked items, and
    // petitions/charters.
    inline bool IsAuctionableItem(ItemTemplate const *t)
    {
        if (!t)
            return false;

        switch (t->Bonding)
        {
        case BIND_WHEN_PICKED_UP:
        case BIND_QUEST_ITEM:
        case BIND_QUEST_ITEM1:
            return false;
        default:
            break;
        }

        if (t->HasFlag(ITEM_FLAG_CONJURED) ||
            t->HasFlag(ITEM_FLAG_NO_PICKUP) ||
            t->HasFlag(ITEM_FLAG_IS_BOUND_TO_ACCOUNT) ||
            t->HasFlag(ITEM_FLAG_PETITION))
            return false;

        return true;
    }

    // --- Post queue (for auction postings) ---
    struct PostRequest
    {
        AuctionHouseId house = AuctionHouseId::Neutral;
        uint32 itemId = 0;
        uint32 count = 1;
        uint32 startBid = 0;
        uint32 buyout = 0;
        uint32 duration = 12 * HOUR;
    };

    class PostQueue
    {
    public:
        void Push(PostRequest r) { _q.emplace_back(std::move(r)); }
        std::vector<PostRequest> Drain(uint32 max)
        {
            if (max == 0 || _q.empty())
                return {};
            uint32 take = std::min<uint32>(max, uint32(_q.size()));
            std::vector<PostRequest> out(_q.begin(), _q.begin() + take);
            _q.erase(_q.begin(), _q.begin() + take);
            return out;
        }
        uint32 Size() const { return uint32(_q.size()); }
        void Clear() { _q.clear(); }

    private:
        std::vector<PostRequest> _q;
    };

    // --- Config keys (one place) ---
    inline constexpr char const *CFG_ENABLE_SELLER = "ModDynamicAH.EnableSeller";
    inline constexpr char const *CFG_DRYRUN = "ModDynamicAH.DryRun";
    inline constexpr char const *CFG_INTERVAL_MIN = "ModDynamicAH.IntervalMin";
    inline constexpr char const *CFG_MAX_RANDOM = "ModDynamicAH.MaxRandomPerCycle";
    inline constexpr char const *CFG_MIN_PRICE = "ModDynamicAH.MinPriceCopper";
    inline constexpr char const *CFG_BLOCK_TRASH = "ModDynamicAH.BlockTrashAndCommon";
    inline constexpr char const *CFG_ALLOW_Q_POOR = "ModDynamicAH.AllowQ.Poor";
    inline constexpr char const *CFG_ALLOW_Q_NORMAL = "ModDynamicAH.AllowQ.Normal";
    inline constexpr char const *CFG_ALLOW_Q_UNCOMMON = "ModDynamicAH.AllowQ.Uncommon";
    inline constexpr char const *CFG_ALLOW_Q_RARE = "ModDynamicAH.AllowQ.Rare";
    inline constexpr char const *CFG_ALLOW_Q_EPIC = "ModDynamicAH.AllowQ.Epic";
    inline constexpr char const *CFG_ALLOW_Q_LEGENDARY = "ModDynamicAH.AllowQ.Legendary";
    inline constexpr char const *CFG_WHITE_ALLOW = "ModDynamicAH.WhiteAllowCSV";
    inline constexpr char const *CFG_NEVER_ABOVE_VENDOR_BUY = "ModDynamicAH.Buy.NeverAboveVendorBuy";
    inline constexpr char const *CFG_CONTEXT_ENABLED = "ModDynamicAH.Context.Enabled";
    inline constexpr char const *CFG_CONTEXT_MAX_PER_BRACKET = "ModDynamicAH.Context.MaxPerBracket";
    inline constexpr char const *CFG_CONTEXT_WEIGHT_BOOST = "ModDynamicAH.Context.WeightBoost";
    inline constexpr char const *CFG_CONTEXT_VENDOR_SKIP = "ModDynamicAH.Context.SkipVendor";
    inline constexpr char const *CFG_SCARCITY_ENABLED = "ModDynamicAH.Scarcity.Enabled";
    inline constexpr char const *CFG_SCARCITY_PRICE_BOOST_MAX = "ModDynamicAH.Scarcity.PriceBoostMax";
    inline constexpr char const *CFG_SCARCITY_PER_TICK_ITEM_CAP = "ModDynamicAH.Scarcity.PerItemCap";
    inline constexpr char const *CFG_VENDOR_MIN_MARKUP = "ModDynamicAH.Vendor.MinMarkup";
    inline constexpr char const *CFG_VENDOR_CONSIDER_BUYPRICE = "ModDynamicAH.Vendor.ConsiderBuyPrice";

    inline constexpr char const *CFG_STACK_DEFAULT = "ModDynamicAH.Stack.Default";
    inline constexpr char const *CFG_STACK_CLOTH = "ModDynamicAH.Stack.Cloth";
    inline constexpr char const *CFG_STACK_ORE = "ModDynamicAH.Stack.Ore";
    inline constexpr char const *CFG_STACK_BAR = "ModDynamicAH.Stack.Bar";
    inline constexpr char const *CFG_STACK_HERB = "ModDynamicAH.Stack.Herb";
    inline constexpr char const *CFG_STACK_LEATHER = "ModDynamicAH.Stack.Leather";
    inline constexpr char const *CFG_STACK_DUST = "ModDynamicAH.Stack.Dust";
    inline constexpr char const *CFG_STACK_GEM = "ModDynamicAH.Stack.Gem";
    inline constexpr char const *CFG_STACK_STONE = "ModDynamicAH.Stack.Stone";
    inline constexpr char const *CFG_STACK_MEAT = "ModDynamicAH.Stack.Meat";
    inline constexpr char const *CFG_STACK_BANDAGE = "ModDynamicAH.Stack.Bandage";
    inline constexpr char const *CFG_STACK_POTION = "ModDynamicAH.Stack.Potion";
    inline constexpr char const *CFG_STACK_INK = "ModDynamicAH.Stack.Ink";
    inline constexpr char const *CFG_STACK_PIGMENT = "ModDynamicAH.Stack.Pigment";
    inline constexpr char const *CFG_STACK_FISH = "ModDynamicAH.Stack.Fish";

    inline constexpr char const *CFG_STACKS_LOW = "ModDynamicAH.Stacks.Low";
    inline constexpr char const *CFG_STACKS_MID = "ModDynamicAH.Stacks.Mid";
    inline constexpr char const *CFG_STACKS_HIGH = "ModDynamicAH.Stacks.High";

    inline constexpr char const *CFG_PRICE_MUL_DUST = "ModDynamicAH.PriceMul.Dust";
    inline constexpr char const *CFG_PRICE_MUL_ESSENCE = "ModDynamicAH.PriceMul.Essence";
    inline constexpr char const *CFG_PRICE_MUL_SHARD = "ModDynamicAH.PriceMul.Shard";
    inline constexpr char const *CFG_PRICE_MUL_ELEMENTAL = "ModDynamicAH.PriceMul.Elemental";
    inline constexpr char const *CFG_PRICE_MUL_RARERAW = "ModDynamicAH.PriceMul.RareRaw";

    inline constexpr char const *CFG_DEBUG_CONTEXT_LOGS = "ModDynamicAH.Context.DebugLogs";
    inline constexpr char const *CFG_LOOP_ENABLED = "ModDynamicAH.Loop.Enabled";

    // economy
    inline constexpr char const *CFG_ECON_GOLD_PER_QUEST = "ModDynamicAH.Econ.AvgGoldPerQuest";
    inline constexpr char const *CFG_ECON_QPF_PREFIX = "ModDynamicAH.Econ.QPF."; // +FamilyName

    // owners
    inline constexpr char const *CFG_SELLER_OWNER_ALLI = "ModDynamicAH.Owner.Alliance";
    inline constexpr char const *CFG_SELLER_OWNER_HORDE = "ModDynamicAH.Owner.Horde";
    inline constexpr char const *CFG_SELLER_OWNER_NEUT = "ModDynamicAH.Owner.Neutral";

    // setup helpers
    inline constexpr char const *CFG_SETUP_ACC_NAME = "ModDynamicAH.Setup.AccountName";
    inline constexpr char const *CFG_SETUP_ACC_PASS = "ModDynamicAH.Setup.AccountPass";
    inline constexpr char const *CFG_SETUP_ALLI_NAME = "ModDynamicAH.Setup.AllianceName";
    inline constexpr char const *CFG_SETUP_HORD_NAME = "ModDynamicAH.Setup.HordeName";
    inline constexpr char const *CFG_SETUP_NEUT_NAME = "ModDynamicAH.Setup.NeutralName";
    inline constexpr char const *CFG_SETUP_ALLI_RACE = "ModDynamicAH.Setup.AllianceRace";
    inline constexpr char const *CFG_SETUP_ALLI_CLASS = "ModDynamicAH.Setup.AllianceClass";
    inline constexpr char const *CFG_SETUP_ALLI_GENDER = "ModDynamicAH.Setup.AllianceGender";
    inline constexpr char const *CFG_SETUP_HORD_RACE = "ModDynamicAH.Setup.HordeRace";
    inline constexpr char const *CFG_SETUP_HORD_CLASS = "ModDynamicAH.Setup.HordeClass";
    inline constexpr char const *CFG_SETUP_HORD_GENDER = "ModDynamicAH.Setup.HordeGender";
    inline constexpr char const *CFG_SETUP_NEUT_RACE = "ModDynamicAH.Setup.NeutralRace";
    inline constexpr char const *CFG_SETUP_NEUT_CLASS = "ModDynamicAH.Setup.NeutralClass";
    inline constexpr char const *CFG_SETUP_NEUT_GENDER = "ModDynamicAH.Setup.NeutralGender";

    // Added config keys from old module for feature parity
    inline constexpr char const *CFG_CONTEXT_MAX_PER_TICK_PLAYER = "ModDynamicAH.Context.MaxPerTickPerPlayer";
    inline constexpr char const *CFG_CAP_ENABLED = "ModDynamicAH.Cap.Enabled";
    inline constexpr char const *CFG_CAP_TOTAL = "ModDynamicAH.Cap.TotalPerCycle";
    inline constexpr char const *CFG_CAP_HOUSE_ALLI = "ModDynamicAH.Cap.House.Alliance";
    inline constexpr char const *CFG_CAP_HOUSE_HORDE = "ModDynamicAH.Cap.House.Horde";
    inline constexpr char const *CFG_CAP_HOUSE_NEUT = "ModDynamicAH.Cap.House.Neutral";
    inline constexpr char const *CFG_CAP_FAMILY_PREFIX = "ModDynamicAH.Cap.Family.";
    inline constexpr char const *CFG_BUY_ENABLED = "ModDynamicAH.Buy.Enabled";
    inline constexpr char const *CFG_BUY_BUDGET_GOLD = "ModDynamicAH.Buy.PerCycleBudgetGold";
    inline constexpr char const *CFG_BUY_MIN_MARGIN = "ModDynamicAH.Buy.MinMargin";
    inline constexpr char const *CFG_BUY_PER_ITEM_CAP = "ModDynamicAH.Buy.PerItemPerCycleCap";
    inline constexpr char const *CFG_BUY_MAX_SCAN_ROWS = "ModDynamicAH.Buy.MaxScanRows";
    inline constexpr char const *CFG_BUY_BLOCK_TRASH_COMMON = "ModDynamicAH.Buy.BlockTrashAndCommon";

    // Parses a comma/space separated list of uint32s into a set
    inline std::unordered_set<uint32_t> ParseCsvU32(std::string const &csv)
    {
        std::unordered_set<uint32_t> out;
        uint64_t cur = 0;
        bool inNum = false;
        for (char ch : csv)
        {
            if (ch >= '0' && ch <= '9')
            {
                cur = (cur * 10) + (ch - '0');
                inNum = true;
            }
            else
            {
                if (inNum)
                {
                    out.insert(static_cast<uint32_t>(cur));
                    cur = 0;
                    inNum = false;
                }
            }
        }
        if (inNum)
            out.insert(static_cast<uint32_t>(cur));
        return out;
    }
}
