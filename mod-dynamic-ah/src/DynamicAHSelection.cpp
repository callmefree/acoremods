#include "DynamicAHSelection.h"
#include "DatabaseEnv.h"
#include "ObjectMgr.h"
#include <random>
#include "GameTime.h"
#include "QueryResult.h"
#include "Field.h"

namespace ModDynamicAH
{

    static bool QualityAllowed(uint32 q, SelectionConfig const &cfg)
    {
        if (q < 6)
            return cfg.allowQuality[q];
        return false;
    }

    std::vector<ItemCandidate> DynamicAHSelection::PickRandomSellables(SelectionConfig const &cfg, uint32 maxCount)
    {
        std::vector<ItemCandidate> pool;

        // A tiny, cheap pool: items with vendor price signal (Buy or Sell), optionally filter by quality.
        if (QueryResult qr = WorldDatabase.Query("SELECT entry, Quality FROM item_template WHERE BuyPrice > 0 OR SellPrice > 0"))
        {
            do
            {
                Field *f = qr->Fetch();
                uint32 id = f[0].Get<uint32>();
                uint32 q = f[1].Get<uint32>();

                ItemTemplate const *t = sObjectMgr->GetItemTemplate(id);
                if (!t)
                    continue;

                // Never list items a player could not auction (BoP, quest, account-bound, etc.).
                if (!IsAuctionableItem(t))
                    continue;

                if (cfg.blockTrashAndCommon && (q <= 1) && cfg.whitelist.find(id) == cfg.whitelist.end())
                    continue;

                if (!QualityAllowed(q, cfg))
                    continue;

                pool.push_back({id, t});
            } while (qr->NextRow());
        }

        if (pool.empty() || maxCount == 0)
            return {};

        // Shuffle and take first K
        std::mt19937 rng(uint32(GameTime::GetGameTime().count()));
        std::shuffle(pool.begin(), pool.end(), rng);
        if (pool.size() > maxCount)
            pool.resize(maxCount);
        return pool;
    }

} // namespace ModDynamicAH
