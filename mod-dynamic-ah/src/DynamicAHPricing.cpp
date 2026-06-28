#include "DynamicAHPricing.h"
#include "MathUtil.h"

namespace ModDynamicAH
{

    PricingResult DynamicAHPricing::Compute(PricingInputs const &in)
    {
        PricingResult r;

        if (!in.tmpl)
            return r;

        // A very basic baseline from vendor SellPrice; avoid zeros
        uint32 vendorSell = std::max<uint32>(in.tmpl->SellPrice, in.tmpl->BuyPrice / 2);
        uint32 base = std::max<uint32>(in.minPriceCopper, vendorSell);

        // Gentle scarcity modulation: fewer active -> higher price
        double scarcity = 1.0 + (in.activeInHouse == 0 ? 0.25 : (0.25 / double(1 + in.activeInHouse)));

        // Online scaling (very low online -> softer prices; high -> slightly higher)
        double pop = 1.0 + std::min(0.30, double(in.onlineCount) / 500.0 * 0.10);

        uint32 startBid = uint32(double(base) * scarcity * pop);
        uint32 buyout = uint32(double(startBid) * 1.45);

        r.startBid = std::max<uint32>(in.minPriceCopper, startBid);
        r.buyout = std::max<uint32>(r.startBid + 1, buyout);
        return r;
    }

} // namespace ModDynamicAH
