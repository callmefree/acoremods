#include "StateGraph.h"
#include "StateGraphCache.h"

namespace LivingWorld::StateGraph
{

Value Get(ActorRef a, ActorRef b, std::string_view key)
{
    return StateGraphMgr::Instance().Get(a, b, key);
}

bool Has(ActorRef a, ActorRef b, std::string_view key)
{
    return StateGraphMgr::Instance().Has(a, b, key);
}

void Set(ActorRef a, ActorRef b, std::string_view key, Value v)
{
    StateGraphMgr::Instance().Set(a, b, key, std::move(v));
}

void Clear(ActorRef a, ActorRef b, std::string_view key)
{
    StateGraphMgr::Instance().Clear(a, b, key);
}

void SetMulti(std::vector<ActorRef> const& actors, ActorRef b,
              std::string_view key, Value v)
{
    // Each actor gets its own edge with the same value. Cheaper than
    // implementing this in StateGraphMgr::Set since the per-actor calls
    // already cover cache update + DB write + Phase 2 enforcement.
    for (ActorRef const& a : actors)
        StateGraphMgr::Instance().Set(a, b, key, v);
}

bool HasAny(std::vector<ActorRef> const& actors, ActorRef b,
            std::string_view key)
{
    for (ActorRef const& a : actors)
        if (StateGraphMgr::Instance().Has(a, b, key))
            return true;
    return false;
}

std::vector<Edge> List(ActorRef a)
{
    return StateGraphMgr::Instance().List(a);
}

std::vector<Edge> Query(QueryFilter const& f)
{
    return StateGraphMgr::Instance().Query(f);
}

void Nuke(ActorRef a)
{
    StateGraphMgr::Instance().Nuke(a);
}

size_t EdgeCount()
{
    return StateGraphMgr::Instance().EdgeCount();
}

void Initialize()
{
    StateGraphMgr::Instance().Initialize();
}

void Shutdown()
{
    StateGraphMgr::Instance().Shutdown();
}

}  // namespace LivingWorld::StateGraph
