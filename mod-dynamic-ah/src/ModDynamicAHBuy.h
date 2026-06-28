#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>

#include <fmt/format.h>       // fmt::format
#include "Chat.h"             // ChatHandler
#include "Log.h"              // LOG_INFO
#include "AuctionHouseMgr.h"  // AuctionHouseId (core type, no redeclare!)
#include "DynamicAHTypes.h"   // shared enums/aliases for the module
#include "DynamicAHPricing.h" // PricingResult

namespace ModDynamicAH
{
    //--------------------------------------------------------------------------------------------------
    // Seller-bot character low GUIDs, used to (a) skip the bot's own listings when buying and
    // (b) know which bot pays for / receives a buyout in each auction house.
    //--------------------------------------------------------------------------------------------------
    struct BotOwners
    {
        uint32_t alliance = 0;
        uint32_t horde = 0;
        uint32_t neutral = 0;

        bool IsBot(uint32_t lowGuid) const
        {
            return lowGuid && (lowGuid == alliance || lowGuid == horde || lowGuid == neutral);
        }

        uint32_t ForHouse(AuctionHouseId house) const
        {
            switch (house)
            {
            case AuctionHouseId::Alliance:
                return alliance;
            case AuctionHouseId::Horde:
                return horde;
            default:
                return neutral;
            }
        }
    };

    //--------------------------------------------------------------------------------------------------
    // Configuration for the buy engine
    //--------------------------------------------------------------------------------------------------
    struct BuyEngineConfig
    {
        bool enabled = false;
        uint64_t budgetCopper = 0;       // per-cycle budget in copper
        float minMargin = 0.15f;         // required discount vs fair buyout (0.15 = 15%)
        uint32_t perItemPerCycleCap = 2; // cap accepted per item per plan cycle
        uint32_t maxScanRows = 2000;
        bool blockTrashAndCommon = true; // block q=poor/common unless allow-listed

        // Vendor safety
        bool vendorConsiderBuyPrice = true;   // treat BuyPrice>0 as vendor-sold
        bool neverAboveVendorBuyPrice = true; // do not buy if unit buyout > vendor BuyPrice
        uint32_t minPriceCopper = 10000;      // fallback min when vendor info missing

        // Context (for pricing)
        bool scarcityEnabled = true;
        uint32_t onlineCount = 0;
    };

    //--------------------------------------------------------------------------------------------------
    // Buy engine: scans AH rows, filters, plans buys, and (for now) traces decisions
    //--------------------------------------------------------------------------------------------------
    class BuyEngine
    {
    public:
        BuyEngine() = default;

        // Config / filters
        void SetConfig(BuyEngineConfig const &cfg) { _cfg = cfg; }
        void SetFilters(bool allowQuality[6], std::unordered_set<uint32_t> const &whiteAllow);
        void SetOwners(uint32_t alliance, uint32_t horde, uint32_t neutral)
        {
            _owners = BotOwners{alliance, horde, neutral};
        }
        void SetDebug(bool on) { _debug = on; } // echo reasons to chat/log

        // Planning (lambdas provided by caller)
        //  - scarceFn:  (itemId, houseId) -> active count of item in that AH
        //  - fairFn:    (itemId, activeCount) -> PricingResult (unit guidance)
        //  - vendorFn:  (itemId) -> { isVendor, vendorBuyPrice }
        void ResetCycle();
        void BuildPlan(
            std::function<uint32_t(uint32_t, AuctionHouseId)> scarceFn,
            std::function<PricingResult(uint32_t, uint32_t)> fairFn,
            std::function<std::pair<bool, uint32_t>(uint32_t)> vendorFn);

        // Apply up to N planned buys. If dryRun=true, only logs. Returns number “applied”.
        uint32_t Apply(uint32_t maxToApply, bool dryRun, ChatHandler *handler);

        // Introspection / commands
        size_t QueueSize() const { return _queue.size(); }
        uint64_t BudgetUsed() const { return _budgetUsed; }
        uint64_t BudgetLimit() const { return _cfg.budgetCopper; }

        void CmdShow(ChatHandler *handler) const;
        void CmdEnable(ChatHandler *handler, bool enable);
        void CmdBudget(ChatHandler *handler, uint32_t gold);
        void CmdMargin(ChatHandler *handler, uint32_t percent);
        void CmdPerItem(ChatHandler *handler, uint32_t cap);
        void CmdOnce(ChatHandler *handler,
                     std::function<uint32_t(uint32_t, AuctionHouseId)> scarceFn,
                     std::function<PricingResult(uint32_t, uint32_t)> fairFn,
                     std::function<std::pair<bool, uint32_t>(uint32_t)> vendorFn);
        void CmdDebug(ChatHandler *handler, bool on);
        // Backwards-compat alias
        void CmdTrace(ChatHandler *handler, bool on) { CmdDebug(handler, on); }

        // Logging helpers
        void LogBuyDecision(char const* phase, uint32_t aucId, uint32_t itemId, uint32_t count,
                            uint32_t unitAskCopper, uint32_t fairUnitCopper, double marginPct,
                            uint32_t budgetRemainCopper, char const* reason) const;
        void LogBuyResult(uint32_t aucId, uint32_t itemId, uint32_t count,
                          uint32_t unitPaidCopper, char const* result) const;

    private:
        struct BuyCandidate
        {
            uint32_t auctionId;
            AuctionHouseId houseId;
            uint32_t itemId;
            uint32_t count;     // stack count
            uint32_t buyout;    // total stack buyout (copper)
            uint32_t startBid;  // total stack start bid
            uint32_t vendorBuy; // vendor BuyPrice (unit), 0 if not vendor
            float margin;       // discount vs fair (0.15 = 15%)
        };

        // Internal helpers
        bool _qualityAllowed(uint32_t itemId) const;
        bool _passesVendorSafety(uint32_t itemId, uint32_t unitBuyout, uint32_t vendorBuy) const;

        // Tracer using {fmt}; visible to both .cpp and callers
        static void _traceWhy(ChatHandler *handler,
                              std::string_view tag,
                              std::string_view msg)
        {
            const std::string s(msg);
            if (handler)
                handler->SendSysMessage(("ModDynamicAH[BUY][" + std::string(tag) + "] " + s).c_str());
            LOG_INFO("mod.dynamicah", "BUY[{}] {}", tag, s);
        }

        template <typename... Args>
        static void _traceWhy(ChatHandler *handler,
                              std::string_view tag,
                              std::string_view fmtStr,
                              Args &&...args)
        {
            const std::string s = fmt::format(fmt::runtime(fmtStr), std::forward<Args>(args)...);
            if (handler)
                handler->SendSysMessage(("ModDynamicAH[BUY][" + std::string(tag) + "] " + s).c_str());
            LOG_INFO("mod.dynamicah", "BUY[{}] {}", tag, s);
        }

        // Execute a single planned buyout against the live auction house (offline bot buyer).
        // Returns true if the auction was bought out and removed.
        bool _executeBuyout(BuyCandidate const &c, ChatHandler *handler);

        BuyEngineConfig _cfg;

        bool _allowQuality[6] = {false, false, true, true, true, false};
        std::unordered_set<uint32_t> _whiteAllow;
        BotOwners _owners;

        // Plan state
        std::vector<BuyCandidate> _queue;
        std::unordered_map<uint32_t, uint32_t> _perItemCount; // itemId -> planned buys in this cycle
        uint64_t _budgetUsed = 0;

        // Debug
        bool _debug = true; // default on: emits LOG_INFO here, and to Chat if handler != nullptr
        mutable uint32_t _chatLinesThisApply = 0;
        static constexpr uint32_t _chatLineCapPerApply = 40;

        // Plan-phase chat echo (only when invoked from .dah buy once)
        ChatHandler *_planEcho = nullptr;
    };

} // namespace ModDynamicAH
