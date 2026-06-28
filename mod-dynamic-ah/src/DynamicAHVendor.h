#pragma once

#include "DynamicAHTypes.h"
#include "DatabaseEnv.h"
#include "ObjectMgr.h"

namespace ModDynamicAH
{

    class DynamicAHVendor
    {
    public:
        // 0 = not vendor, 1 = limited stock, 2 = unlimited
        static uint8 VendorStockType(uint32 itemId, ItemTemplate const *tmpl, bool considerBuyPrice);
        static void ApplyVendorFloor(ItemTemplate const *tmpl, uint32 &startBid, uint32 &buyout, uint32 minPriceCopper, double vendorMinMarkup);

    private:
        static std::unordered_map<uint32, uint8> &Cache();
    };

} // namespace ModDynamicAH
