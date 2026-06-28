// Phase 4 consequence-layer NPC: Lia, Marra's daughter at the docks.
// Permanent DB spawn, gossip-only. Her dialogue varies based on
// whether anyone in the viewing player's membership tree has resolved
// the Dockside Friend arc on the Help path.
//
// Why she always exists (rather than only spawning post-help): in
// 3.3.5a, per-viewer creature visibility is non-trivial. The cheaper
// "world-change" demonstration is to make her gossip per-viewer
// reactive — the creature is always present, but what she says
// depends on whether your group remembers paying her mother's debt.
// See docs/living_world_inspirations.md §6 for the bend-not-rebuild
// principle.

#include "../statestore/StateGraph.h"

#include "Creature.h"
#include "CreatureScript.h"
#include "Log.h"
#include "Player.h"
#include "ScriptedGossip.h"

namespace
{

// npc_text rows added by rev_1777593600000000007.sql.
constexpr uint32 TEXT_DEFAULT = 80113;
constexpr uint32 TEXT_HELPED  = 80114;

// Phase 4 arc flag — same key Allison/Arn write.
constexpr char const* FLAG_HELPED = "arc.dockfriend.helped";

// Mirrors the helper in Allison/Arn — Player + Guild membership tree.
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

class npc_lia_living_world : public CreatureScript
{
public:
    npc_lia_living_world() : CreatureScript("npc_lia_living_world") {}

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        ClearGossipMenuFor(player);

        using namespace LivingWorld::StateGraph;
        ActorRef global = ActorRef::Abstract("global");
        auto     actors = PlayerMembershipActors(player);

        bool helped = HasAny(actors, global, FLAG_HELPED);
        uint32 textId = helped ? TEXT_HELPED : TEXT_DEFAULT;

        LOG_INFO("module",
                 "mod-living-world: lia.OnGossipHello "
                 "player='{}' (guid={}, guild={}) "
                 "helped={} -> textId={}",
                 player->GetName(), player->GetGUID().GetCounter(),
                 player->GetGuildId(), helped, textId);

        SendGossipMenuFor(player, textId, creature->GetGUID());
        return true;
    }
};

}  // namespace

void AddLiaLivingWorldScript()
{
    new npc_lia_living_world();
}
