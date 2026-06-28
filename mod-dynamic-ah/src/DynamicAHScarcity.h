#pragma once

#include "DynamicAHTypes.h"
#include "WorldSessionMgr.h"
#include "DatabaseEnv.h"

namespace ModDynamicAH
{

    class DynamicAHScarcity
    {
    public:
        void Rebuild();
        uint32 Count(uint32 itemId, AuctionHouseId house) const;
        uint32 OnlineCount() const { return _online; }
        void Clear();

    private:
        std::unordered_map<uint64, uint32> _active; // key = (house << 32) | itemId
        uint32 _online = 0;
    };

} // namespace ModDynamicAH
