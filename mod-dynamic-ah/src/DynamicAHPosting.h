#pragma once

#include "DynamicAHTypes.h"
class ChatHandler;

namespace ModDynamicAH
{
    struct ModuleState; // forward declaration

    class DynamicAHPosting
    {
    public:
        // Selects the owner character GUID for the given house
        static ObjectGuid OwnerGuidFor(ModuleState const &s, AuctionHouseId house);

        // Apply vendor floor to start/buyout based on template and policy
        static void VendorFloor(uint32 minPriceCopper, double vendorMinMarkup, ItemTemplate const *tmpl,
                                uint32 &startBid, uint32 &buyout, bool considerBuyPrice);

        static void ApplyPlanOnWorld(ModuleState &s, uint32 maxToApply, ChatHandler *handler);

        static bool PostSingleAuction(ModuleState const &ctx,
                                      AuctionHouseId house,
                                      uint32 itemId, uint32 count,
                                      uint32 startBid, uint32 buyout,
                                      uint32 durationSeconds,
                                      ChatHandler *handler);

        static bool PostSingleAuction(ModuleState const &ctx,
                                      AuctionHouseId house,
                                      uint32 itemId, uint32 count,
                                      uint32 startBid, uint32 buyout,
                                      uint32 durationSeconds,
                                      ChatHandler *handler,
                                      CharacterDatabaseTransaction &trans);
    };
} // namespace ModDynamicAH
