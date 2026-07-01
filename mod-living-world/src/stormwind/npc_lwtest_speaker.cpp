// Phase 2 reactive-layer plumbing: a single throwaway test NPC whose
// gossip menu varies based on a per-guild StateGraph flag.
//
// This proves the read/write loop end-to-end:
//   - OnGossipHello reads `flag.lwtest_speaker_visited` from the player's
//     guild edge and picks one of two menu items.
//   - OnGossipSelect flips that flag.
//   - The variation persists across worldserver restart (StateGraph is
//     write-through to CharacterDatabase).
//
// Throwaway by design — replace with real authored content in Phase 3.
// See docs/phase_2_demo.md for the manual verification procedure.

#include "../statestore/StateGraph.h"

#include "Creature.h"
#include "CreatureScript.h"
#include "Log.h"
#include "Player.h"
#include "ScriptedGossip.h"

namespace
{

constexpr char const* FLAG_KEY = "flag.lwtest_speaker_visited";

// Phase 2 test plumbing uses a constant guild id so any character —
// guilded, guildless, GM, etc. — interacts with the same flag and sees
// the read/write loop without setup. Real Phase 3 NPCs will use
// `player->GetGuildId()` properly per the per-guild production model.
constexpr uint32 TEST_GUILD_ID = 1;

constexpr uint32 ACTION_GREET  = GOSSIP_ACTION_INFO_DEF + 1;
constexpr uint32 ACTION_FORGET = GOSSIP_ACTION_INFO_DEF + 2;

// npc_text id 1 is the system-default greeting present in the base
// world DB ("Greetings, $N." style). Using it lets us avoid a
// migration for a custom npc_text row — the variation lives in the
// menu items, not the body.
constexpr uint32 BODY_TEXT_ID = 1;

class npc_lwtest_speaker : public CreatureScript
{
public:
    npc_lwtest_speaker() : CreatureScript("npc_lwtest_speaker") {}

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        ClearGossipMenuFor(player);

        using namespace LivingWorld::StateGraph;
        ActorRef guild  = ActorRef::Guild(TEST_GUILD_ID);
        ActorRef global = ActorRef::Abstract("global");
        bool visited = Get(guild, global, FLAG_KEY).AsBool();

        char const* variant = visited ? "forget" : "greet";
        LOG_INFO("module",
                 "mod-living-world: lwtest_speaker.OnGossipHello "
                 "player='{}' (guid={}) read flag={} -> showing variant='{}'",
                 player->GetName(), player->GetGUID().GetCounter(),
                 visited ? "true" : "false", variant);

        if (visited)
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                             "我们之前谈过。忘了我们吧。",
                             GOSSIP_SENDER_MAIN, ACTION_FORGET);
        }
        else
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                             "你好，陌生人。记住我们的公会。",
                             GOSSIP_SENDER_MAIN, ACTION_GREET);
        }

        SendGossipMenuFor(player, BODY_TEXT_ID, creature->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* /*creature*/,
                        uint32 /*sender*/, uint32 action) override
    {
        ClearGossipMenuFor(player);
        CloseGossipMenuFor(player);

        using namespace LivingWorld::StateGraph;
        ActorRef guild  = ActorRef::Guild(TEST_GUILD_ID);
        ActorRef global = ActorRef::Abstract("global");

        char const* actionName = "?";
        bool        newValue   = false;

        if (action == ACTION_GREET)
        {
            actionName = "GREET";
            newValue   = true;
            Set(guild, global, FLAG_KEY, Value::OfBool(true));
        }
        else if (action == ACTION_FORGET)
        {
            actionName = "FORGET";
            newValue   = false;
            Set(guild, global, FLAG_KEY, Value::OfBool(false));
        }
        else
        {
            LOG_WARN("module",
                     "mod-living-world: lwtest_speaker.OnGossipSelect "
                     "player='{}' got unexpected action={}",
                     player->GetName(), action);
            return true;
        }

        LOG_INFO("module",
                 "mod-living-world: lwtest_speaker.OnGossipSelect "
                 "player='{}' (guid={}) action='{}' -> wrote flag={}",
                 player->GetName(), player->GetGUID().GetCounter(),
                 actionName, newValue ? "true" : "false");

        return true;
    }
};

}  // namespace

void AddAllisonLivingWorldScript();
void AddArnBrennanLivingWorldScript();
void AddLiaLivingWorldScript();

void AddLivingWorldStormwindScripts()
{
    new npc_lwtest_speaker();
    AddAllisonLivingWorldScript();
    AddArnBrennanLivingWorldScript();
    AddLiaLivingWorldScript();
}
