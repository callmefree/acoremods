#include "DynamicAHScarcity.h"
#include "World.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"  // for ResultSet, Field

namespace ModDynamicAH
{

    void DynamicAHScarcity::Clear()
    {
        _active.clear();
        _online = 0;
    }

    void DynamicAHScarcity::Rebuild()
    {
        _active.clear();

        if (QueryResult r = CharacterDatabase.Query(
                "SELECT ii.itemEntry, ah.houseid, COUNT(*) "
                "FROM auctionhouse ah JOIN item_instance ii ON ii.guid = ah.itemguid "
                "GROUP BY ii.itemEntry, ah.houseid"))
        {
            do
            {
                Field *f = r->Fetch();
                uint32 item = f[0].Get<uint32>();
                uint32 house = f[1].Get<uint32>();
                uint32 cnt = f[2].Get<uint32>();
                uint64 key = (uint64(house) << 32) | item;
                _active[key] = cnt;
            } while (r->NextRow());
        }

        _online = static_cast<uint32>(sWorldSessionMgr->GetActiveSessionCount());
    }

    uint32 DynamicAHScarcity::Count(uint32 itemId, AuctionHouseId house) const
    {
        uint64 key = (uint64(uint32(house)) << 32) | itemId;
        auto it = _active.find(key);
        return it != _active.end() ? it->second : 0u;
    }

} // namespace ModDynamicAH