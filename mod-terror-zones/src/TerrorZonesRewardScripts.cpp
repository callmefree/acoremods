#include "TerrorZonesMgr.h"

#include "Creature.h"
#include "LootMgr.h"
#include "Player.h"
#include "ScriptMgr.h"

using namespace mod_terror_zones;

namespace
{
    class TerrorZones_RewardPlayerScript : public PlayerScript
    {
    public:
        TerrorZones_RewardPlayerScript()
            : PlayerScript("TerrorZones_RewardPlayerScript") {}

        void OnPlayerGiveXP(Player* player, uint32& amount,
                            Unit* /*victim*/, uint8 /*xpSource*/) override
        {
            TerrorZonesMgr::Instance().ApplyXpMultiplier(amount, player);
        }

        void OnPlayerBeforeLootMoney(Player* player, Loot* loot) override
        {
            if (!loot)
                return;
            TerrorZonesMgr::Instance().ApplyGoldMultiplier(loot->gold, player);
        }

        // Event-boss gold uplift fires here. OnPlayerCreatureKill
        // runs at Unit.cpp:14309 — well AFTER both FillLoot
        // (Unit.cpp:14081) and generateMoneyLoot (Unit.cpp:14084),
        // so our mutation of creature->loot.gold sticks for the
        // loot window opening. The earlier OnPlayerBeforeLootMoney
        // path was unreliable because if the gold pile showed a
        // small native amount, players ignored it instead of
        // clicking — and our uplift only fired on click.
        void OnPlayerCreatureKill(Player* killer,
                                   Creature* killed) override
        {
            if (!killer || !killed)
                return;
            TerrorZonesMgr::Instance().ApplyEventBossGoldUplift(
                killed->loot, killer);
        }

        void OnPlayerQuestComputeMoney(Player* player, Quest const* /*quest*/,
                                       int32& moneyRew) override
        {
            TerrorZonesMgr::Instance().ApplyQuestGoldMultiplier(moneyRew,
                                                                 player);
        }
    };

    class TerrorZones_RewardGlobalScript : public GlobalScript
    {
    public:
        TerrorZones_RewardGlobalScript()
            : GlobalScript("TerrorZones_RewardGlobalScript") {}

        void OnBeforeDropAddItem(Player const* player, Loot& loot,
                                 bool /*canRate*/, uint16 /*lootMode*/,
                                 LootStoreItem* item,
                                 LootStore const& store) override
        {
            if (!player)
                return;
            auto& mgr = TerrorZonesMgr::Instance();
            // Slice 3 — tier-bump substitution.
            mgr.TryTierBump(player, item);
            // Slice 4 — Prospector's gathering yield overlay.
            mgr.TryGatheringYieldBump(player, item, store.GetName());
            // Slice 4 — additive unique drop (per-bundle, not per-item).
            mgr.TryUniqueDrop(player, &loot, player->GetZoneId());
            // Slice 6 — additive event-boss bonus drop (no-op in MVP,
            // Pass-2 seam).
            mgr.TryEventBossDrop(player, loot);
        }
    };
}

void AddTerrorZonesRewardScripts()
{
    new TerrorZones_RewardPlayerScript();
    new TerrorZones_RewardGlobalScript();
}
