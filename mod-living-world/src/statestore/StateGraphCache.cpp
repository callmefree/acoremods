#include "StateGraphCache.h"

#include "Config.h"
#include "Log.h"

#include <algorithm>

namespace LivingWorld::StateGraph
{

// Defined in StateGraphPersistence.cpp.
uint64 DbInsertAbstractActor(std::string const& name);

namespace
{
    char const* ActorTypeName(ActorType t)
    {
        switch (t)
        {
            case ActorType::None:               return "none";
            case ActorType::Guild:              return "guild";
            case ActorType::Player:             return "player";
            case ActorType::Faction:            return "faction";
            case ActorType::Abstract:           return "abstract";
            case ActorType::CreatureTemplate:   return "creature_template";
            case ActorType::GameObjectTemplate: return "gameobject_template";
            case ActorType::Location:           return "location";
            case ActorType::CreatureSpawn:      return "creature_spawn";
            case ActorType::GameObjectSpawn:    return "gameobject_spawn";
            case ActorType::ItemInstance:       return "item_instance";
        }
        return "unknown";
    }
}

StateGraphMgr& StateGraphMgr::Instance()
{
    static StateGraphMgr inst;
    return inst;
}

void StateGraphMgr::LoadConfig()
{
    _enabled   = sConfigMgr->GetOption<bool>("LivingWorld.Enable", true);
    _debug     = sConfigMgr->GetOption<bool>("LivingWorld.Debug", false);
    // Default ON during the prototype phase so persistence is visible
    // in worldserver logs without extra config. Flip back to false once
    // real Phase 3+ content makes per-Set logs noisy.
    _logWrites = sConfigMgr->GetOption<bool>("LivingWorld.StateGraph.LogWritesToDb", true);
}

void StateGraphMgr::Initialize()
{
    if (_initialized)
        return;

    DbLoadAbstractActors();
    _initialized = true;

    LOG_INFO("module",
             "mod-living-world: StateGraph initialized "
             "(abstract actors loaded: {}, enabled: {}).",
             _abstractByName.size(), _enabled ? "yes" : "no");
}

void StateGraphMgr::Shutdown()
{
    // Slice 1 writes synchronously, so nothing to flush. Reset state for
    // the next initialize cycle (e.g., during a reload).
    _edges.clear();
    _loadedActors.clear();
    _abstractByName.clear();
    _abstractById.clear();
    _initialized = false;
    LOG_INFO("module", "mod-living-world: StateGraph shutdown.");
}

bool StateGraphMgr::CheckWritePhase1(ActorRef const& a, ActorRef const& b,
                                     char const* operation) const
{
    // State graph Phase 2: relaxed to allow Guild *or* Player on the
    // a-side, with Abstract on the b-side. The membership-tree model
    // (player + guild simultaneously) needs Player as a writable actor.
    bool aOk = (a.type == ActorType::Guild || a.type == ActorType::Player);
    bool bOk = (b.type == ActorType::Abstract);
    if (aOk && bOk)
        return true;

    LOG_WARN("module",
             "mod-living-world: rejecting {} for unsupported actor pair "
             "(a={}, b={}). Phase 2 supports {{Guild,Player}} <-> Abstract.",
             operation, ActorTypeName(a.type), ActorTypeName(b.type));
    return false;
}

uint64 StateGraphMgr::ResolveAbstract(std::string_view name)
{
    if (!_initialized)
    {
        LOG_WARN("module",
                 "mod-living-world: ResolveAbstract('{}') called before "
                 "Initialize(). Returning 0.", std::string{name});
        return 0;
    }

    std::string key{name};
    auto it = _abstractByName.find(key);
    if (it != _abstractByName.end())
        return it->second;

    // Insert a new abstract actor row, holding the lock through the DB
    // call. Slow but correct under contention; slice 2 may revisit if
    // the abstract-actor write path becomes hot.
    uint64 newId = DbInsertAbstractActor(key);
    if (newId == 0)
        return 0;

    _abstractByName.emplace(key, newId);
    _abstractById.emplace(newId, key);

    if (_debug)
        LOG_INFO("module",
                 "mod-living-world: registered new abstract actor '{}' = id {}.",
                 key, newId);
    return newId;
}

void StateGraphMgr::EnsureLoaded(ActorRef const& a)
{
    if (!a.IsValid())
        return;
    ActorKey k{a.type, a.id};
    if (_loadedActors.find(k) != _loadedActors.end())
        return;
    DbLoadEdgesFor(a);
    _loadedActors.insert(k);
}

Value StateGraphMgr::Get(ActorRef a, ActorRef b, std::string_view key)
{
    if (!a.IsValid() || !b.IsValid())
        return Value{};

    EnsureLoaded(a);

    EdgeKey ek{a.type, a.id, b.type, b.id, std::string{key}};
    auto it = _edges.find(ek);
    if (it == _edges.end())
        return Value{};
    return it->second.value;
}

bool StateGraphMgr::Has(ActorRef a, ActorRef b, std::string_view key)
{
    if (!a.IsValid() || !b.IsValid())
        return false;
    EnsureLoaded(a);
    EdgeKey ek{a.type, a.id, b.type, b.id, std::string{key}};
    return _edges.find(ek) != _edges.end();
}

void StateGraphMgr::Set(ActorRef a, ActorRef b, std::string_view key, Value v)
{
    if (!_enabled)
        return;
    if (!a.IsValid() || !b.IsValid())
        return;
    if (!CheckWritePhase1(a, b, "Set"))
        return;

    EnsureLoaded(a);

    Edge e;
    e.a         = a;
    e.b         = b;
    e.key       = std::string{key};
    e.value     = std::move(v);
    e.updatedAt = ::time(nullptr);

    EdgeKey ek{a.type, a.id, b.type, b.id, e.key};
    _edges[ek] = e;

    DbWriteEdge(e);

    if (_logWrites)
    {
        LOG_INFO("module",
                 "mod-living-world: Set ({}/{} <-> {}/{}) '{}' = {} (type={})",
                 ActorTypeName(a.type), a.id,
                 ActorTypeName(b.type), b.id,
                 e.key, e.value.AsString(),
                 static_cast<uint32>(e.value.type));
    }
}

void StateGraphMgr::Clear(ActorRef a, ActorRef b, std::string_view key)
{
    if (!_enabled)
        return;
    if (!a.IsValid() || !b.IsValid())
        return;
    if (!CheckWritePhase1(a, b, "Clear"))
        return;

    EnsureLoaded(a);

    std::string keyStr{key};
    EdgeKey ek{a.type, a.id, b.type, b.id, keyStr};
    _edges.erase(ek);
    DbDeleteEdge(a, b, keyStr);

    if (_logWrites)
    {
        LOG_INFO("module",
                 "mod-living-world: Clear ({}/{} <-> {}/{}) '{}'",
                 ActorTypeName(a.type), a.id,
                 ActorTypeName(b.type), b.id, keyStr);
    }
}

std::vector<Edge> StateGraphMgr::List(ActorRef a)
{
    std::vector<Edge> out;
    if (!a.IsValid())
        return out;
    EnsureLoaded(a);
    for (auto const& kv : _edges)
    {
        Edge const& e = kv.second;
        if (e.a == a || e.b == a)
            out.push_back(e);
    }
    return out;
}

std::vector<Edge> StateGraphMgr::Query(QueryFilter const& f)
{
    std::vector<Edge> out;
    if (f.actorA)
        EnsureLoaded(*f.actorA);
    if (f.actorB)
        EnsureLoaded(*f.actorB);

    for (auto const& kv : _edges)
    {
        Edge const& e = kv.second;
        if (f.actorA && e.a != *f.actorA)
            continue;
        if (f.actorB && e.b != *f.actorB)
            continue;
        if (f.key && e.key != *f.key)
            continue;
        out.push_back(e);
    }
    return out;
}

void StateGraphMgr::Nuke(ActorRef a)
{
    if (!a.IsValid())
        return;

    DbDeleteAllForActor(a);

    // Remove from in-memory cache
    for (auto it = _edges.begin(); it != _edges.end(); )
    {
        if (it->second.a == a || it->second.b == a)
            it = _edges.erase(it);
        else
            ++it;
    }
    _loadedActors.erase(ActorKey{a.type, a.id});

    LOG_INFO("module",
             "mod-living-world: Nuked all edges for actor (type={}, id={}).",
             ActorTypeName(a.type), a.id);
}

size_t StateGraphMgr::EdgeCount()
{
    return _edges.size();
}

}  // namespace LivingWorld::StateGraph
