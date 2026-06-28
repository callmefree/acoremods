#pragma once

#include "DynamicAHTypes.h"

namespace ModDynamicAH
{

    struct PricingInputs
    {
        ItemTemplate const *tmpl = nullptr;
        uint32 activeInHouse = 0; // how many active auctions of this item in that house
        uint32 onlineCount = 0;   // online players (scarcity proxy)
        uint32 minPriceCopper = 10000;
    };

    struct PricingResult
    {
        uint32 startBid = 0;
        uint32 buyout = 0;
    };

    class DynamicAHPricing
    {
    public:
        static PricingResult Compute(PricingInputs const &in);
    };

} // namespace ModDynamicAH
