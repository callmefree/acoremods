// Phase 4 second NPC: Arn Brennan, dock hand at the Stormwind canal.
// He's the discovery surface for the "Dockside Friend" arc — a player
// who hasn't been sent by Allison gets a one-line dismissal; a player
// with `arc.dockfriend.hook_given` set unlocks Marra's story and the
// three resolution paths.
//
// Anchors: Vimes (Discworld — observational, no patience for fools),
// Eileen the Crow (Bloodborne — older, knew people who are gone),
// the dock-side workers in The Wire — wary by default, frank when
// they decide you're worth talking to.

#include "../statestore/StateGraph.h"

#include "Chat.h"
#include "Creature.h"
#include "CreatureScript.h"
#include "DBCStores.h"
#include "Log.h"
#include "Player.h"
#include "ReputationMgr.h"
#include "ScriptedGossip.h"
#include "WorldSession.h"

namespace
{

// npc_text rows added by rev_1777593600000000006.sql.
constexpr uint32 TEXT_DEFAULT   = 80108;
constexpr uint32 TEXT_WITH_HOOK = 80109;
constexpr uint32 TEXT_STORY     = 80110;
constexpr uint32 TEXT_HELPED    = 80111;
constexpr uint32 TEXT_TOLD      = 80112;

// Arc flags. Namespaced under `arc.dockfriend.*` so future arcs don't
// collide. Bool flags rather than an enum so HasAny works directly.
constexpr char const* FLAG_HOOK_GIVEN = "arc.dockfriend.hook_given";
constexpr char const* FLAG_AT_DOCKS   = "arc.dockfriend.at_docks";
constexpr char const* FLAG_HELPED     = "arc.dockfriend.helped";
constexpr char const* FLAG_TOLD       = "arc.dockfriend.told";

// 50 gold in copper (1g = 100s = 10000c).
constexpr int32 DEBT_PAYMENT_COPPER = 50 * 10000;

// Phase 4 consequence-layer rewards on the Help path.
// Bend WoW primitives: existing item_template + reputation system.
constexpr uint32 ITEM_HOUSE_POUR        = 800200;
constexpr uint32 HOUSE_POUR_GIVE_COUNT  = 3;
constexpr uint32 STORMWIND_FACTION_ID   = 72;
constexpr int32  REP_GAIN_HELPED        = 250;

// Action ids — keep clear of Allison's range and the vanilla
// GOSSIP_ACTION_TRADE / GOSSIP_ACTION_INN.
constexpr uint32 ACTION_NAME_ALLISON = GOSSIP_ACTION_INFO_DEF + 200;
constexpr uint32 ACTION_PAY_DEBT     = GOSSIP_ACTION_INFO_DEF + 201;
constexpr uint32 ACTION_TELL_ALLISON = GOSSIP_ACTION_INFO_DEF + 202;
constexpr uint32 ACTION_WALK_AWAY    = GOSSIP_ACTION_INFO_DEF + 203;

enum class Branch
{
    Default,    // no hook — random walk-up
    WithHook,   // Allison sent you, first time at Arn
    Story,      // post-revelation, three choices available
    Helped,     // resolved by paying
    Told,       // resolved by telling Allison
};

char const* BranchName(Branch b)
{
    switch (b)
    {
        case Branch::Default:  return "default";
        case Branch::WithHook: return "with_hook";
        case Branch::Story:    return "story";
        case Branch::Helped:   return "helped";
        case Branch::Told:     return "told";
    }
    return "?";
}

// Mirrors Allison's helper. Player + Guild (if any). Future: Party/Raid.
std::vector<LivingWorld::StateGraph::ActorRef>
PlayerMembershipActors(Player* player)
{
    using namespace LivingWorld::StateGraph;
    std::vector<ActorRef> actors;
    actors.reserve(2);
    actors.push_back(ActorRef::Player(player->GetGUID().GetCounter()));
    if (uint32 g = player->GetGuildId())
        actors.push_back(ActorRef::Guild(g));
    return actors;
}

class npc_arn_brennan_living_world : public CreatureScript
{
public:
    npc_arn_brennan_living_world() : CreatureScript("npc_arn_brennan_living_world") {}

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        SendCurrentMenu(player, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature,
                        uint32 /*sender*/, uint32 action) override
    {
        ClearGossipMenuFor(player);

        using namespace LivingWorld::StateGraph;
        ActorRef global = ActorRef::Abstract("global");
        auto     actors = PlayerMembershipActors(player);

        switch (action)
        {
            case ACTION_NAME_ALLISON:
                SetMulti(actors, global, FLAG_AT_DOCKS, Value::OfBool(true));
                LOG_INFO("module",
                         "mod-living-world: arn.OnGossipSelect "
                         "player='{}' action='NAME_ALLISON' wrote='{}'",
                         player->GetName(), FLAG_AT_DOCKS);
                SendCurrentMenu(player, creature);
                return true;

            case ACTION_PAY_DEBT:
            {
                if (player->GetMoney() < static_cast<uint32>(DEBT_PAYMENT_COPPER))
                {
                    creature->Whisper(
                        "You haven't the coin. Don't waste my time.",
                        LANG_UNIVERSAL, player);
                    LOG_INFO("module",
                             "mod-living-world: arn.OnGossipSelect "
                             "player='{}' action='PAY_DEBT' "
                             "rejected (insufficient money: {})",
                             player->GetName(), player->GetMoney());
                    SendCurrentMenu(player, creature);
                    return true;
                }
                player->ModifyMoney(-DEBT_PAYMENT_COPPER);
                SetMulti(actors, global, FLAG_HELPED, Value::OfBool(true));

                // Consequence layer: artifact + faction-rep shift,
                // both via existing WoW primitives. See
                // docs/living_world_inspirations.md §4.2.
                bool itemOk = player->AddItem(ITEM_HOUSE_POUR,
                                              HOUSE_POUR_GIVE_COUNT);
                if (!itemOk)
                    LOG_WARN("module",
                             "mod-living-world: arn could not give "
                             "item {} to player='{}' (inventory full?)",
                             ITEM_HOUSE_POUR, player->GetName());

                if (FactionEntry const* faction =
                        sFactionStore.LookupEntry(STORMWIND_FACTION_ID))
                    player->GetReputationMgr().ModifyReputation(
                        faction, REP_GAIN_HELPED);

                LOG_INFO("module",
                         "mod-living-world: arn.OnGossipSelect "
                         "player='{}' action='PAY_DEBT' wrote='{}' "
                         "(deducted {} copper, gave item={} x{}, "
                         "stormwind rep +{})",
                         player->GetName(), FLAG_HELPED,
                         DEBT_PAYMENT_COPPER,
                         ITEM_HOUSE_POUR, HOUSE_POUR_GIVE_COUNT,
                         REP_GAIN_HELPED);
                SendCurrentMenu(player, creature);
                return true;
            }

            case ACTION_TELL_ALLISON:
                LOG_INFO("module",
                         "mod-living-world: arn.OnGossipSelect "
                         "player='{}' action='TELL_ALLISON' "
                         "(no flag — go talk to Allison)",
                         player->GetName());
                CloseGossipMenuFor(player);
                return true;

            case ACTION_WALK_AWAY:
                LOG_INFO("module",
                         "mod-living-world: arn.OnGossipSelect "
                         "player='{}' action='WALK_AWAY'",
                         player->GetName());
                CloseGossipMenuFor(player);
                return true;

            default:
                LOG_WARN("module",
                         "mod-living-world: arn unexpected action={}",
                         action);
                CloseGossipMenuFor(player);
                return true;
        }
    }

private:
    void SendCurrentMenu(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);

        using namespace LivingWorld::StateGraph;
        ActorRef global = ActorRef::Abstract("global");
        auto     actors = PlayerMembershipActors(player);

        bool hookGiven = HasAny(actors, global, FLAG_HOOK_GIVEN);
        bool atDocks   = HasAny(actors, global, FLAG_AT_DOCKS);
        bool helped    = HasAny(actors, global, FLAG_HELPED);
        bool told      = HasAny(actors, global, FLAG_TOLD);

        Branch branch = Branch::Default;
        if (helped)         branch = Branch::Helped;
        else if (told)      branch = Branch::Told;
        else if (atDocks)   branch = Branch::Story;
        else if (hookGiven) branch = Branch::WithHook;

        LOG_INFO("module",
                 "mod-living-world: arn.SendCurrentMenu "
                 "player='{}' (guid={}, guild={}) "
                 "flags=(hook={}, at_docks={}, helped={}, told={}) "
                 "-> branch='{}'",
                 player->GetName(), player->GetGUID().GetCounter(),
                 player->GetGuildId(),
                 hookGiven, atDocks, helped, told,
                 BranchName(branch));

        uint32 textId = TEXT_DEFAULT;
        switch (branch)
        {
            case Branch::Default:
                textId = TEXT_DEFAULT;
                // No options — he dismisses you.
                break;

            case Branch::WithHook:
                textId = TEXT_WITH_HOOK;
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                                 "是艾莉森让我来的。",
                                 GOSSIP_SENDER_MAIN, ACTION_NAME_ALLISON);
                break;

            case Branch::Story:
                textId = TEXT_STORY;
                AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG,
                                 "这50金币我来出。",
                                 GOSSIP_SENDER_MAIN, ACTION_PAY_DEBT);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                                 "我得先跟艾莉森谈谈。",
                                 GOSSIP_SENDER_MAIN, ACTION_TELL_ALLISON);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                                 "[转身离开。]",
                                 GOSSIP_SENDER_MAIN, ACTION_WALK_AWAY);
                break;

            case Branch::Helped:
                textId = TEXT_HELPED;
                // Steady state, no options.
                break;

            case Branch::Told:
                textId = TEXT_TOLD;
                // Steady state, no options.
                break;
        }

        SendGossipMenuFor(player, textId, creature->GetGUID());
    }
};

}  // namespace

void AddArnBrennanLivingWorldScript()
{
    new npc_arn_brennan_living_world();
}
