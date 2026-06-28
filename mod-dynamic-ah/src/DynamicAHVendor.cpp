#include "DynamicAHVendor.h"
#include "DatabaseEnv.h"
#include "ObjectMgr.h"
#include "QueryResult.h"
#include "Field.h"

namespace ModDynamicAH
{

    static std::unordered_map<uint32, uint8> g_vendorCache;
    std::unordered_map<uint32, uint8> &DynamicAHVendor::Cache() { return g_vendorCache; }

    uint8 DynamicAHVendor::VendorStockType(uint32 itemId, ItemTemplate const *tmpl, bool considerBuyPrice)
    {
        auto it = g_vendorCache.find(itemId);
        if (it != g_vendorCache.end())
            return it->second;

        uint8 res = 0;

        if (QueryResult qr = WorldDatabase.Query("SELECT maxcount FROM npc_vendor WHERE item = {} LIMIT 1", itemId))
        {
            int32 maxc = qr->Fetch()[0].Get<int32>(); // 0 = unlimited, >0 = limited
            res = (maxc == 0) ? 2 : 1;
        }
        else if (considerBuyPrice && tmpl && tmpl->BuyPrice > 0)
            res = 2;

        g_vendorCache.emplace(itemId, res);
        return res;
    }

    void DynamicAHVendor::ApplyVendorFloor(ItemTemplate const *tmpl, uint32 &startBid, uint32 &buyout, uint32 minPriceCopper, double vendorMinMarkup)
    {
        if (!tmpl)
            return;

        if (tmpl->BuyPrice == 0)
        {
            startBid = std::max(startBid, minPriceCopper);
            buyout = std::max(buyout, std::max(startBid, minPriceCopper));
            return;
        }

        uint32 floorV = std::max<uint32>(minPriceCopper, uint32(double(tmpl->BuyPrice) * (1.0 + vendorMinMarkup)));
        startBid = std::max(startBid, floorV);
        buyout = std::max(buyout, std::max(startBid, floorV));
    }

} // namespace ModDynamicAH
