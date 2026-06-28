// Phase 3 first deep NPC: Innkeeper Allison (entry 6740) at the
// Gilded Rose, Stormwind. Five branches keyed to four flags form a
// social-investment chain.
//
// Anchors: Delphine (Skyrim — innkeeper with hidden depth), Eileen the
// Crow (Bloodborne — older woman with a no), Kim Kitsuragi (Disco
// Elysium — observational, anchoring), Vimes (Discworld — ethical
// without preaching). Allison is "looks like an innkeeper, *is* an
// innkeeper, also has a no she will defend."
//
// Memory model: events write to both the player's individual edge AND
// each group container they belong to at trigger time (currently
// guild; party/raid is later). Reads OR across the membership tree —
// Allison reacts if the player remembers, OR the guild remembers, OR
// any group container remembers. See StateGraph.h.
//
// Vanilla innkeeper function (vendor + hearth bind + quest giver) is
// replicated here so Allison still functions for players who ignore
// the deep gossip — per living_world_prototype.md §3.3.

#include "../statestore/StateGraph.h"

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

// Allison's vanilla creature entry is 6740 — patched to ScriptName
// 'npc_allison_living_world' by rev_1777593600000000004.sql so this
// script handles her gossip end-to-end.

// Phase 3 npc_text rows added by rev_1777593600000000004.sql.
constexpr uint32 TEXT_BRANCH_STRANGER  = 80100;
constexpr uint32 TEXT_BRANCH_RETURNING = 80101;
constexpr uint32 TEXT_BRANCH_CURIOUS   = 80102;
constexpr uint32 TEXT_BRANCH_THE_NO    = 80103;
constexpr uint32 TEXT_BRANCH_REGULAR   = 80104;

// Phase 4 arc npc_text rows added by rev_1777593600000000006.sql.
constexpr uint32 TEXT_BRANCH_HOOK_GIVEN = 80105;
constexpr uint32 TEXT_BRANCH_HELPED     = 80106;
constexpr uint32 TEXT_BRANCH_TOLD       = 80107;

// Phase 3 flags. Namespaced so future NPCs don't collide.
constexpr char const* FLAG_VISITED      = "allison.visited";
constexpr char const* FLAG_HEARD_NEWS   = "allison.heard_news";
constexpr char const* FLAG_ASKED_DOCKS  = "allison.asked_about_docks";
constexpr char const* FLAG_RECOGNIZED   = "allison.recognized";

// Phase 4 arc flags — shared with Arn Brennan's script.
constexpr char const* FLAG_ARC_HOOK_GIVEN = "arc.dockfriend.hook_given";
constexpr char const* FLAG_ARC_AT_DOCKS   = "arc.dockfriend.at_docks";
constexpr char const* FLAG_ARC_HELPED     = "arc.dockfriend.helped";
constexpr char const* FLAG_ARC_TOLD       = "arc.dockfriend.told";

// Vanilla innkeeper menu — pull text from the existing gossip_menu_option
// rows so we don't reauthor "Make this inn your home." etc.
constexpr uint32 INNKEEPER_GOSSIP_MENU = 9733;
constexpr uint32 INNKEEPER_OPT_INN     = 1;  // hearth-bind
constexpr uint32 INNKEEPER_OPT_TRADE   = 2;  // vendor

// Action ids for our deep gossip. Keep clear of the vanilla
// GOSSIP_ACTION_TRADE / GOSSIP_ACTION_INN values, and clear of Arn's
// 200-range.
constexpr uint32 ACTION_LOOK_AROUND        = GOSSIP_ACTION_INFO_DEF + 100;
constexpr uint32 ACTION_ASK_NEWS           = GOSSIP_ACTION_INFO_DEF + 101;
constexpr uint32 ACTION_WHY_NOT_DOCKS      = GOSSIP_ACTION_INFO_DEF + 102;
constexpr uint32 ACTION_TELL_HARBOR        = GOSSIP_ACTION_INFO_DEF + 103;  // shallow
constexpr uint32 ACTION_DROP_IT            = GOSSIP_ACTION_INFO_DEF + 104;
// Phase 4 arc actions
constexpr uint32 ACTION_TELL_ABOUT_FRIEND  = GOSSIP_ACTION_INFO_DEF + 105;
constexpr uint32 ACTION_HOOK_ACK           = GOSSIP_ACTION_INFO_DEF + 106;
constexpr uint32 ACTION_TELL_DAUGHTER      = GOSSIP_ACTION_INFO_DEF + 107;

// Phase 4 consequence-layer rewards on the Tell path. Smaller artifact
// and rep gain than Help (no gold cost; she's giving you a keepsake,
// not paying off a debt). Bend WoW item + reputation primitives.
constexpr uint32 ITEM_TOKEN              = 800201;
constexpr uint32 STORMWIND_FACTION_ID    = 72;
constexpr int32  REP_GAIN_TOLD           = 100;

enum class Branch
{
    Stranger,
    Returning,
    Curious,
    TheNo,
    Regular,
    HookGiven,   // arc: she just told you about Marra; go find Arn
    AtDocks,     // arc: you've been to Arn; she doesn't know yet
    Helped,      // arc: terminal — you paid Marra's debt
    Told,        // arc: terminal — you came back and told her
};

char const* BranchName(Branch b)
{
    switch (b)
    {
        case Branch::Stranger:   return "stranger";
        case Branch::Returning:  return "returning";
        case Branch::Curious:    return "curious";
        case Branch::TheNo:      return "the_no";
        case Branch::Regular:    return "regular";
        case Branch::HookGiven:  return "hook_given";
        case Branch::AtDocks:    return "at_docks";
        case Branch::Helped:     return "helped";
        case Branch::Told:       return "told";
    }
    return "?";
}

// Build the membership tree for `player`: their individual actor plus
// every group container they belong to right now (only guild for the
// moment; party/raid is a later extension). Order doesn't matter for
// reads, but the player goes first for consistent log output.
std::vector<LivingWorld::StateGraph::ActorRef> PlayerMembershipActors(Player* player)
{
    using namespace LivingWorld::StateGraph;
    std::vector<ActorRef> actors;
    actors.reserve(2);
    actors.push_back(ActorRef::Player(player->GetGUID().GetCounter()));
    if (uint32 g = player->GetGuildId())
        actors.push_back(ActorRef::Guild(g));
    return actors;
}

class npc_allison_living_world : public CreatureScript
{
public:
    npc_allison_living_world() : CreatureScript("npc_allison_living_world") {}

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

        // Vanilla innkeeper handlers — these switch to other UI, so
        // close the gossip window naturally.
        if (action == GOSSIP_ACTION_TRADE)
        {
            CloseGossipMenuFor(player);
            player->GetSession()->SendListInventory(creature->GetGUID());
            return true;
        }
        if (action == GOSSIP_ACTION_INN)
        {
            CloseGossipMenuFor(player);
            player->SetBindPoint(creature->GetGUID());
            return true;
        }

        // Deep gossip — write to the full membership tree so every
        // actor (player + guild + future party/raid) carries the memory.
        char const* actionName  = "?";
        char const* flagWritten = nullptr;
        switch (action)
        {
            case ACTION_LOOK_AROUND:
                SetMulti(actors, global, FLAG_VISITED, Value::OfBool(true));
                actionName  = "LOOK_AROUND";
                flagWritten = FLAG_VISITED;
                break;

            case ACTION_ASK_NEWS:
                SetMulti(actors, global, FLAG_HEARD_NEWS, Value::OfBool(true));
                actionName  = "ASK_NEWS";
                flagWritten = FLAG_HEARD_NEWS;
                break;

            case ACTION_WHY_NOT_DOCKS:
                SetMulti(actors, global, FLAG_ASKED_DOCKS, Value::OfBool(true));
                actionName  = "WHY_NOT_DOCKS";
                flagWritten = FLAG_ASKED_DOCKS;
                break;

            case ACTION_DROP_IT:
                // The right move — she rewards letting the no stand.
                SetMulti(actors, global, FLAG_RECOGNIZED, Value::OfBool(true));
                actionName  = "DROP_IT";
                flagWritten = FLAG_RECOGNIZED;
                break;

            case ACTION_TELL_HARBOR:
                // Shallow path — no flag, no progression. Re-render at
                // the same branch so the player can pick again or close.
                actionName = "TELL_HARBOR_SHALLOW";
                break;

            case ACTION_TELL_ABOUT_FRIEND:
                // Phase 4 arc hook: she names Marra and points to Arn.
                SetMulti(actors, global, FLAG_ARC_HOOK_GIVEN, Value::OfBool(true));
                actionName  = "TELL_ABOUT_FRIEND";
                flagWritten = FLAG_ARC_HOOK_GIVEN;
                break;

            case ACTION_HOOK_ACK:
                // Player acknowledges the hook ("I'll go."). No flag —
                // they need to actually visit Arn for at_docks to be set.
                actionName = "HOOK_ACK";
                break;

            case ACTION_TELL_DAUGHTER:
                // Phase 4 arc resolution: player visited Arn, returned,
                // and is telling Allison the daughter is alive.
                SetMulti(actors, global, FLAG_ARC_TOLD, Value::OfBool(true));

                // Consequence layer: smaller artifact + rep shift.
                if (!player->AddItem(ITEM_TOKEN, 1))
                    LOG_WARN("module",
                             "mod-living-world: allison could not give "
                             "item {} to player='{}' (inventory full?)",
                             ITEM_TOKEN, player->GetName());

                if (FactionEntry const* faction =
                        sFactionStore.LookupEntry(STORMWIND_FACTION_ID))
                    player->GetReputationMgr().ModifyReputation(
                        faction, REP_GAIN_TOLD);

                actionName  = "TELL_DAUGHTER";
                flagWritten = FLAG_ARC_TOLD;
                break;

            default:
                LOG_WARN("module",
                         "mod-living-world: allison unexpected action={}",
                         action);
                CloseGossipMenuFor(player);
                return true;
        }

        LOG_INFO("module",
                 "mod-living-world: allison.OnGossipSelect "
                 "player='{}' action='{}'{}{}",
                 player->GetName(), actionName,
                 flagWritten ? " wrote='" : "",
                 flagWritten ? flagWritten : "");

        // Continuous dialogue: re-render the menu in place. The player
        // sees the next branch (or the same branch's text refreshed)
        // without needing to right-click the NPC again.
        SendCurrentMenu(player, creature);
        return true;
    }

private:
    void SendCurrentMenu(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);

        using namespace LivingWorld::StateGraph;
        ActorRef global = ActorRef::Abstract("global");
        auto     actors = PlayerMembershipActors(player);

        // Phase 3 flags
        bool visited    = HasAny(actors, global, FLAG_VISITED);
        bool heardNews  = HasAny(actors, global, FLAG_HEARD_NEWS);
        bool askedDocks = HasAny(actors, global, FLAG_ASKED_DOCKS);
        bool recognized = HasAny(actors, global, FLAG_RECOGNIZED);

        // Phase 4 arc flags
        bool arcHook    = HasAny(actors, global, FLAG_ARC_HOOK_GIVEN);
        bool arcAtDocks = HasAny(actors, global, FLAG_ARC_AT_DOCKS);
        bool arcHelped  = HasAny(actors, global, FLAG_ARC_HELPED);
        bool arcTold    = HasAny(actors, global, FLAG_ARC_TOLD);

        // Branch determination — most specific (resolution) first.
        Branch branch = Branch::Stranger;
        if (arcHelped)             branch = Branch::Helped;
        else if (arcTold)          branch = Branch::Told;
        else if (arcAtDocks)       branch = Branch::AtDocks;
        else if (arcHook)          branch = Branch::HookGiven;
        else if (recognized)       branch = Branch::Regular;
        else if (askedDocks)       branch = Branch::TheNo;
        else if (heardNews)        branch = Branch::Curious;
        else if (visited)          branch = Branch::Returning;

        LOG_INFO("module",
                 "mod-living-world: allison.SendCurrentMenu "
                 "player='{}' (guid={}, guild={}) "
                 "phase3=(v={}, n={}, d={}, r={}) "
                 "arc=(h={}, ad={}, hp={}, t={}) "
                 "-> branch='{}'",
                 player->GetName(), player->GetGUID().GetCounter(),
                 player->GetGuildId(),
                 visited, heardNews, askedDocks, recognized,
                 arcHook, arcAtDocks, arcHelped, arcTold,
                 BranchName(branch));

        // Vanilla innkeeper function — preserve drink/room/quest in
        // every branch so non-engaged players still get an innkeeper.
        if (creature->IsQuestGiver())
            player->PrepareQuestMenu(creature->GetGUID());
        if (creature->IsVendor())
            AddGossipItemFor(player, INNKEEPER_GOSSIP_MENU, INNKEEPER_OPT_TRADE,
                             GOSSIP_SENDER_MAIN, GOSSIP_ACTION_TRADE);
        if (creature->IsInnkeeper())
            AddGossipItemFor(player, INNKEEPER_GOSSIP_MENU, INNKEEPER_OPT_INN,
                             GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INN);

        player->TalkedToCreature(creature->GetEntry(), creature->GetGUID());

        // Branch-specific deep gossip option(s) and body text.
        uint32 textId = TEXT_BRANCH_STRANGER;
        switch (branch)
        {
            case Branch::Stranger:
                textId = TEXT_BRANCH_STRANGER;
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                                 "Just looking around for now.",
                                 GOSSIP_SENDER_MAIN, ACTION_LOOK_AROUND);
                break;

            case Branch::Returning:
                textId = TEXT_BRANCH_RETURNING;
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                                 "What's the news in the city?",
                                 GOSSIP_SENDER_MAIN, ACTION_ASK_NEWS);
                break;

            case Branch::Curious:
                textId = TEXT_BRANCH_CURIOUS;
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                                 "Why not the docks?",
                                 GOSSIP_SENDER_MAIN, ACTION_WHY_NOT_DOCKS);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                                 "Tell me more about the harbor.",
                                 GOSSIP_SENDER_MAIN, ACTION_TELL_HARBOR);
                break;

            case Branch::TheNo:
                textId = TEXT_BRANCH_THE_NO;
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                                 "[Drop the subject.]",
                                 GOSSIP_SENDER_MAIN, ACTION_DROP_IT);
                break;

            case Branch::Regular:
                textId = TEXT_BRANCH_REGULAR;
                // Phase 4 arc hook — only show if not yet given.
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                                 "Tell me about your friend at the docks.",
                                 GOSSIP_SENDER_MAIN, ACTION_TELL_ABOUT_FRIEND);
                break;

            case Branch::HookGiven:
                textId = TEXT_BRANCH_HOOK_GIVEN;
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                                 "I'll go.",
                                 GOSSIP_SENDER_MAIN, ACTION_HOOK_ACK);
                break;

            case Branch::AtDocks:
                // Reuse the Returning body — she doesn't reference the
                // arc until you bring it up. The new option is the
                // difference.
                textId = TEXT_BRANCH_RETURNING;
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                                 "I went down to the docks. Marra's daughter is still there.",
                                 GOSSIP_SENDER_MAIN, ACTION_TELL_DAUGHTER);
                break;

            case Branch::Helped:
                textId = TEXT_BRANCH_HELPED;
                // Steady state, no options — vanilla function only.
                break;

            case Branch::Told:
                textId = TEXT_BRANCH_TOLD;
                // Steady state, no options — vanilla function only.
                break;
        }

        SendGossipMenuFor(player, textId, creature->GetGUID());
    }
};

}  // namespace

void AddAllisonLivingWorldScript()
{
    new npc_allison_living_world();
}
