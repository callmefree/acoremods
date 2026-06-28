#include "ModDynamicAHBuy.h"

#include "Item.h"
#include "ObjectMgr.h"
#include "World.h"
#include "AuctionHouseMgr.h" // AuctionHouseObject, AuctionEntry, sAuctionMgr
#include "SharedDefines.h"
#include "Log.h"   // ITEM_CLASS_TRADE_GOODS
#include "DatabaseEnv.h" // CharacterDatabase, transactions
#include "ObjectGuid.h"  // ObjectGuid, HighGuid

using namespace ModDynamicAH;

// -------------------------------------------------------------------------------------------------
// Filters / helpers
// -------------------------------------------------------------------------------------------------

static inline std::string MoneyShort(uint32 copper)
{
    uint32 g = copper / 10000;
    uint32 s = (copper / 100) % 100;
    uint32 c = copper % 100;
    std::string out;
    if (g)
        out += fmt::format("{}g", g);
    if (s)
        out += fmt::format("{}s", s);
    if (c || out.empty())
        out += fmt::format("{}c", c);
    return out;
}

void BuyEngine::SetFilters(bool allowQuality[6], std::unordered_set<uint32_t> const &whiteAllow)
{
    for (int i = 0; i < 6; ++i)
        _allowQuality[i] = allowQuality[i];
    _whiteAllow = whiteAllow;
}

void BuyEngine::ResetCycle()
{
    _queue.clear();
    _perItemCount.clear();
    _budgetUsed = 0;
}

bool BuyEngine::_qualityAllowed(uint32_t itemId) const
{
    ItemTemplate const *t = sObjectMgr->GetItemTemplate(itemId);
    if (!t)
        return false;

    // Always allow explicitly allow-listed white/gray items
    if (t->Quality <= ITEM_QUALITY_NORMAL && _whiteAllow.find(itemId) != _whiteAllow.end())
        return true;

    // Trade Goods (profession mats) are allowed even if white/gray.
    if (t->Class == ITEM_CLASS_TRADE_GOODS)
        return true;

    // If configured to block poor/common, then block them unless allow-listed
    if ((t->Quality <= ITEM_QUALITY_NORMAL) && _cfg.blockTrashAndCommon)
        return false;

    if (t->Quality > 5) // safety
        return false;

    return _allowQuality[t->Quality];
}

bool BuyEngine::_passesVendorSafety(uint32_t /*itemId*/, uint32_t unitBuyout, uint32_t vendorBuy) const
{
    if (!_cfg.neverAboveVendorBuyPrice)
        return true;
    if (!_cfg.vendorConsiderBuyPrice)
        return true;

    // If vendorBuy known and > 0, ensure we never buy above it (per unit)
    if (vendorBuy > 0 && unitBuyout > vendorBuy)
        return false;

    return true;
}

// -------------------------------------------------------------------------------------------------
// Planning (scan in-memory auctions; no SQL)
// -------------------------------------------------------------------------------------------------

void BuyEngine::BuildPlan(
    std::function<uint32_t(uint32_t, AuctionHouseId)> scarceFn,
    std::function<PricingResult(uint32_t, uint32_t)> fairFn,
    std::function<std::pair<bool, uint32_t>(uint32_t)> vendorFn)
{
    if (!_cfg.enabled)
    {
        LOG_INFO("mod.dynamicah", "[BUY] Disabled; skipping build");
        return;
    }

    uint32_t scanned = 0, considered = 0, accepted = 0, skipped = 0;
    uint32_t scanLimit = _cfg.maxScanRows ? _cfg.maxScanRows : 1000;

    auto scanHouse = [&](AuctionHouseId houseId)
    {
        AuctionHouseObject *ahObj = sAuctionMgr->GetAuctionsMapByHouseId(houseId);
        if (!ahObj)
            return;

        auto const &map = ahObj->GetAuctions();
        for (auto const &kv : map)
        {
            if (scanned >= scanLimit)
                break;

            AuctionEntry const *A = kv.second;
            if (!A)
                continue;

            ++scanned;

            uint32_t auctionId = A->Id;
            uint32_t itemId = A->item_template; // set on post; present in AuctionEntry
            uint32_t count = A->itemCount ? A->itemCount : 1u;
            uint32_t buyout = A->buyout;     // total stack buyout
            uint32_t startBid = A->startbid; // total stack startBid

            // Never buy the seller bot's own listings (would just churn gold).
            if (_owners.IsBot(A->owner.GetCounter()))
            {
                ++skipped;
                _traceWhy(_planEcho, "SKIP", "auc={} item={} reason=own-listing", auctionId, itemId);
                continue;
            }

            if (!buyout)
            {
                ++skipped;
                _traceWhy(_planEcho, "SKIP", "auc={} item={} reason=no-buyout", auctionId, itemId);
                continue;
            }

            ItemTemplate const *tmpl = sObjectMgr->GetItemTemplate(itemId);
            const char *itemName = tmpl ? tmpl->Name1.c_str() : "unknown";

            if (!tmpl)
            {
                ++skipped;
                _traceWhy(_planEcho, "SKIP", "auc={} item={} reason=no-template", auctionId, itemId);
                continue;
            }

            // Quality filter
            if (!_qualityAllowed(itemId))
            {
                ++skipped;
                _traceWhy(_planEcho, "SKIP",
                          "auc={} item={} '{}' quality={} filtered",
                          auctionId, itemId, itemName, uint32_t(tmpl->Quality));
                continue;
            }

            ++considered;

            // Scarcity for fair price calculation
            uint32_t activeCount = scarceFn ? scarceFn(itemId, houseId) : 0;
            PricingResult fair = fairFn ? fairFn(itemId, activeCount) : PricingResult{0, 0};

            // Compute "fair value" for this stack (prefer buyout guidance per unit if available)
            uint32_t fairUnit = fair.buyout ? fair.buyout : std::max<uint32_t>(_cfg.minPriceCopper, tmpl->SellPrice * 2);
            uint32_t fairStack = fairUnit * count;

            // Vendor safety
            uint32_t vendorBuy = 0;
            if (vendorFn)
            {
                auto v = vendorFn(itemId);
                vendorBuy = v.second;
            }
            uint32_t unitBuyout = (count ? (buyout / count) : buyout);
            if (!_passesVendorSafety(itemId, unitBuyout, vendorBuy))
            {
                ++skipped;
                _traceWhy(_planEcho, "SKIP",
                          "auc={} item={} '{}' reason=vendor-safety unitBuyout={} ({}) vendorBuy={} ({})",
                          auctionId, itemId, itemName,
                          unitBuyout, MoneyShort(unitBuyout),
                          vendorBuy, MoneyShort(vendorBuy));
                continue;
            }

            // Margin check (how much cheaper vs fair)
            float margin = 0.0f;
            if (fairStack > 0 && buyout < fairStack)
                margin = float(fairStack - buyout) / float(fairStack);

            if (margin < _cfg.minMargin)
            {
                ++skipped;
                _traceWhy(_planEcho, "SKIP",
                          "auc={} item={} '{}' reason=margin-too-small margin={:.1f}% need>={:.1f}% buyout={} ({}) fairStack={} ({})",
                          auctionId, itemId, itemName,
                          margin * 100.0f, _cfg.minMargin * 100.0f,
                          buyout, MoneyShort(buyout),
                          fairStack, MoneyShort(fairStack));
                continue;
            }

            // Per-item per-cycle cap
            uint32_t &plannedForItem = _perItemCount[itemId];
            if (plannedForItem >= _cfg.perItemPerCycleCap)
            {
                ++skipped;
                _traceWhy(_planEcho, "SKIP",
                          "auc={} item={} '{}' reason=per-item-cap cap={}",
                          auctionId, itemId, itemName, _cfg.perItemPerCycleCap);
                continue;
            }

            // Budget check
            if (_budgetUsed + buyout > _cfg.budgetCopper)
            {
                ++skipped;
                _traceWhy(_planEcho, "SKIP",
                          "auc={} item={} '{}' reason=budget-exceeded buyout={} ({}) used={} ({}) limit={} ({})",
                          auctionId, itemId, itemName,
                          buyout, MoneyShort(buyout),
                          _budgetUsed, MoneyShort(uint32(_budgetUsed)),
                          _cfg.budgetCopper, MoneyShort(uint32(_cfg.budgetCopper)));
                continue;
            }

            // Accept
            BuyCandidate bc;
            bc.auctionId = auctionId;
            bc.houseId = houseId;
            bc.itemId = itemId;
            bc.count = count;
            bc.buyout = buyout;
            bc.startBid = startBid;
            bc.vendorBuy = vendorBuy;
            bc.margin = margin;

            _queue.emplace_back(bc);
            ++plannedForItem;
            _budgetUsed += buyout;
            ++accepted;

            _traceWhy(_planEcho, "ACCEPT",
                      "auc={} item={} '{}' x{} unitBuyout={} ({}) fairUnit={} ({}) margin={:.1f}% house={}",
                      auctionId, itemId, itemName, count,
                      unitBuyout, MoneyShort(unitBuyout),
                      fairUnit, MoneyShort(fairUnit),
                      margin * 100.0f, static_cast<uint32_t>(houseId));
            LogBuyDecision("enqueue", auctionId, itemId, count, unitBuyout, fairUnit,
                           (fairUnit ? (double(fairUnit) - double(unitBuyout)) * 100.0 / double(fairUnit) : 0.0),
                           uint32(_budgetUsed), "ok");


            if (scanned >= scanLimit)
                break;
        }
    };

    // Scan houses until we hit scanLimit
    scanHouse(AuctionHouseId::Alliance);
    if (scanned < scanLimit)
        scanHouse(AuctionHouseId::Horde);
    if (scanned < scanLimit)
        scanHouse(AuctionHouseId::Neutral);

    LOG_INFO("mod.dynamicah", "[BUY] scanned={} considered={} accepted={} skipped={} queue={} budget={}/{}",
             scanned, considered, accepted, skipped,
             _queue.size(),
             static_cast<unsigned long long>(_budgetUsed),
             static_cast<unsigned long long>(_cfg.budgetCopper));
}

// -------------------------------------------------------------------------------------------------
// Apply
// -------------------------------------------------------------------------------------------------

uint32_t BuyEngine::Apply(uint32_t maxToApply, bool dryRun, ChatHandler *handler)
{
    if (!_cfg.enabled)
        return 0;

    _chatLinesThisApply = 0;

    // Each planned candidate is consumed exactly once: drain from the front of the queue so a
    // candidate is never re-attempted on a later tick (which would re-scan already-bought
    // auctions and, in dry-run, re-log the whole plan every world update).
    uint32_t applied = 0;
    uint32_t processed = 0;
    for (BuyCandidate const &c : _queue)
    {
        if (processed >= maxToApply)
            break;
        ++processed;

        if (dryRun)
        {
            _traceWhy(handler, "DRY", "auc={} item={} x{} buyout={} margin={:.1f}% house={} vendorBuy={}",
                      c.auctionId, c.itemId, c.count, c.buyout, c.margin * 100.0f,
                      static_cast<uint32_t>(c.houseId), c.vendorBuy);
            LogBuyResult(c.auctionId, c.itemId, c.count, (c.count ? c.buyout / c.count : c.buyout), "ok-dry");
            ++applied;
        }
        else if (_executeBuyout(c, handler))
        {
            ++applied;
        }
    }

    if (processed)
        _queue.erase(_queue.begin(), _queue.begin() + processed);

    return applied;
}

// Buy out a single planned auction on behalf of the (offline) seller bot of that house.
// Mirrors the core buyout path (WorldSession::HandleAuctionPlaceBid): the seller is paid via
// SendAuctionSuccessfulMail, the bot's gold is debited, and the listing is removed. The bought
// item is absorbed (deleted) rather than mailed to the bot, so the bot acts as a market maker
// that soaks up under-priced supply and puts a floor under prices.
bool BuyEngine::_executeBuyout(BuyCandidate const &c, ChatHandler *handler)
{
    uint32_t botLow = _owners.ForHouse(c.houseId);
    if (!botLow)
    {
        _traceWhy(handler, "LIVE-SKIP", "auc={} reason=no-bot-owner house={}",
                  c.auctionId, static_cast<uint32_t>(c.houseId));
        return false;
    }

    AuctionHouseObject *ahObj = sAuctionMgr->GetAuctionsMapByHouseId(c.houseId);
    if (!ahObj)
        return false;

    AuctionEntry *auction = ahObj->GetAuction(c.auctionId);
    if (!auction)
    {
        // Listing vanished between planning and applying (sold/cancelled/expired).
        _traceWhy(handler, "LIVE-SKIP", "auc={} reason=gone", c.auctionId);
        return false;
    }

    // Re-validate against the plan: the auction must still be the same item at the same buyout.
    if (auction->item_template != c.itemId || auction->buyout != c.buyout)
    {
        _traceWhy(handler, "LIVE-SKIP", "auc={} reason=changed", c.auctionId);
        return false;
    }

    ObjectGuid botGuid = ObjectGuid::Create<HighGuid::Player>(botLow);
    if (auction->owner == botGuid)
        return false; // never buy our own listing

    // The bot pays from its real wallet; if it can't afford this stack, stop trying to buy more
    // (the queue is roughly cheapest-first within budget, but the wallet is the hard limit).
    if (QueryResult r = CharacterDatabase.Query("SELECT money FROM characters WHERE guid = {}", botLow))
    {
        uint32 botMoney = r->Fetch()[0].Get<uint32>();
        if (botMoney < auction->buyout)
        {
            _traceWhy(handler, "LIVE-SKIP", "auc={} reason=insufficient-funds have={} need={}",
                      c.auctionId, botMoney, auction->buyout);
            return false;
        }
    }
    else
    {
        _traceWhy(handler, "LIVE-SKIP", "auc={} reason=no-bot-character guid={}", c.auctionId, botLow);
        return false;
    }

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    // Debit the buyer bot.
    trans->Append("UPDATE characters SET money = money - {} WHERE guid = {}", auction->buyout, botLow);

    // Record the bot as the winning bidder at full buyout, then pay the seller their proceeds.
    auction->bidder = botGuid;
    auction->bid = auction->buyout;
    sAuctionMgr->SendAuctionSuccessfulMail(auction, trans);

    // Absorb the item and remove the auction (both in-memory and in DB).
    auction->DeleteFromDB(trans);
    sAuctionMgr->RemoveAItem(auction->item_guid, true, &trans);
    ahObj->RemoveAuction(auction); // frees `auction`

    CharacterDatabase.CommitTransaction(trans);

    _traceWhy(handler, "LIVE-BUY", "auc={} item={} x{} buyout={} margin={:.1f}% house={} bot={}",
              c.auctionId, c.itemId, c.count, c.buyout, c.margin * 100.0f,
              static_cast<uint32_t>(c.houseId), botLow);
    LogBuyResult(c.auctionId, c.itemId, c.count, (c.count ? c.buyout / c.count : c.buyout), "bought");
    return true;
}

// -------------------------------------------------------------------------------------------------
// Commands
// -------------------------------------------------------------------------------------------------

void BuyEngine::CmdShow(ChatHandler *handler) const
{
    if (handler)
    {
        handler->PSendSysMessage(
            "ModDynamicAH[BUY]: enabled={} budget={}/{} cap/item={} minMargin={:.1f}% scanLimit={} debug={} queue={}",
            _cfg.enabled ? "1" : "0",
            static_cast<unsigned long long>(_budgetUsed),
            static_cast<unsigned long long>(_cfg.budgetCopper),
            _cfg.perItemPerCycleCap,
            _cfg.minMargin * 100.0f,
            _cfg.maxScanRows,
            _debug ? "1" : "0",
            _queue.size());
    }
    LOG_INFO("mod.dynamicah",
             "[BUY] enabled={} budget={}/{} cap/item={} minMargin={:.1f}% scanLimit={} debug={} queue={}",
             _cfg.enabled, _budgetUsed, _cfg.budgetCopper, _cfg.perItemPerCycleCap,
             _cfg.minMargin * 100.0f, _cfg.maxScanRows, _debug, _queue.size());
}

void BuyEngine::CmdEnable(ChatHandler *handler, bool enable)
{
    _cfg.enabled = enable;
    if (handler)
        handler->PSendSysMessage("ModDynamicAH[BUY]: {}", enable ? "enabled" : "disabled");
}

void BuyEngine::CmdBudget(ChatHandler *handler, uint32_t gold)
{
    _cfg.budgetCopper = uint64_t(gold) * 10000ull;
    if (handler)
        handler->PSendSysMessage("ModDynamicAH[BUY]: budget set to {} gold", gold);
}

void BuyEngine::CmdMargin(ChatHandler *handler, uint32_t percent)
{
    _cfg.minMargin = std::max(0u, std::min(percent, 95u)) / 100.0f;
    if (handler)
        handler->PSendSysMessage("ModDynamicAH[BUY]: min margin set to {}%", percent);
}

void BuyEngine::CmdPerItem(ChatHandler *handler, uint32_t cap)
{
    _cfg.perItemPerCycleCap = std::max<uint32_t>(1u, cap);
    if (handler)
        handler->PSendSysMessage("ModDynamicAH[BUY]: per-item/cycle cap set to {}", _cfg.perItemPerCycleCap);
}

void BuyEngine::CmdOnce(ChatHandler *handler,
                        std::function<uint32_t(uint32_t, AuctionHouseId)> scarceFn,
                        std::function<PricingResult(uint32_t, uint32_t)> fairFn,
                        std::function<std::pair<bool, uint32_t>(uint32_t)> vendorFn)
{
    ResetCycle();
    _planEcho = handler; // echo reasons for this one run
    BuildPlan(scarceFn, fairFn, vendorFn);
    _planEcho = nullptr;

    uint32_t did = Apply(50, /*dryRun=*/true, handler);
    if (handler)
        handler->PSendSysMessage("ModDynamicAH[BUY]: dry-run would apply ~{} buys (queue={})", did, _queue.size());
}

void BuyEngine::CmdDebug(ChatHandler *handler, bool on)
{
    _debug = on;
    if (handler)
        handler->PSendSysMessage("ModDynamicAH[BUY]: debug {}", on ? "enabled" : "disabled");
}


void BuyEngine::LogBuyDecision(char const* phase, uint32_t aucId, uint32_t itemId, uint32_t count,
                               uint32_t unitAskCopper, uint32_t fairUnitCopper, double marginPct,
                               uint32_t budgetRemainCopper, char const* reason) const
{
    if (!_debug) return;
    ItemTemplate const* t = sObjectMgr->GetItemTemplate(itemId);
    LOG_INFO("mod_dynamic_ah",
             "buy {}: auc={} item={} '{}' x{} ask={}c fair={}c margin={:.1f}% budgetRemain={}c reason={}",
             phase, aucId, itemId, (t ? t->Name1 : std::string("")), count,
             unitAskCopper, fairUnitCopper, marginPct, budgetRemainCopper, (reason ? reason : ""));
}

void BuyEngine::LogBuyResult(uint32_t aucId, uint32_t itemId, uint32_t count,
                             uint32_t unitPaidCopper, char const* result) const
{
    if (!_debug) return;
    ItemTemplate const* t = sObjectMgr->GetItemTemplate(itemId);
    LOG_INFO("mod_dynamic_ah",
             "buy result: auc={} item={} '{}' x{} unitPaid={}c result={}",
             aucId, itemId, (t ? t->Name1 : std::string("")), count, unitPaidCopper, (result ? result : ""));
}
