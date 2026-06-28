#ifndef MOD_LIVING_WORLD_STATE_GRAPH_CACHE_H
#define MOD_LIVING_WORLD_STATE_GRAPH_CACHE_H

#include "StateGraph.h"

#include <unordered_map>
#include <unordered_set>

// Internal cache + manager singleton. Not part of the public API
// (consumers use the free functions in LivingWorld::StateGraph).
//
// THREADING: main worldserver thread only. All ScriptMgr hooks
// (WorldScript, PlayerScript, CreatureScript, chat commands) fire on
// the main tick, so the cache is single-writer/single-reader and needs
// no locking. Calling from a custom thread is a programming error.
//
// Phase 1 is synchronous write-through: Set() updates the cache and
// writes to CharacterDatabase before returning. Slice 2 will add
// debounced batching per docs/living_world_state_graph_design.md §7.

namespace LivingWorld::StateGraph
{

struct ActorKey
{
    ActorType type;
    uint64    id;

    bool operator==(ActorKey const& o) const { return type == o.type && id == o.id; }
};

struct ActorKeyHash
{
    size_t operator()(ActorKey const& k) const noexcept
    {
        return std::hash<uint64>{}(k.id) ^
               (static_cast<size_t>(k.type) * 0x9E3779B97F4A7C15ULL);
    }
};

struct EdgeKey
{
    ActorType   aType;
    uint64      aId;
    ActorType   bType;
    uint64      bId;
    std::string key;

    bool operator==(EdgeKey const& o) const
    {
        return aType == o.aType && aId == o.aId &&
               bType == o.bType && bId == o.bId && key == o.key;
    }
};

struct EdgeKeyHash
{
    size_t operator()(EdgeKey const& k) const noexcept
    {
        size_t h = std::hash<uint64>{}(k.aId);
        h ^= std::hash<uint64>{}(k.bId) + 0x9E3779B9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(k.key) + 0x9E3779B9 + (h << 6) + (h >> 2);
        h ^= static_cast<size_t>(k.aType) * 31u;
        h ^= static_cast<size_t>(k.bType) * 131u;
        return h;
    }
};

class StateGraphMgr
{
public:
    static StateGraphMgr& Instance();

    void LoadConfig();
    void Initialize();   // load abstract actors lookup
    void Shutdown();     // flush any pending writes (slice 2); reset state

    bool IsEnabled() const { return _enabled; }
    bool IsDebug()   const { return _debug; }
    bool LogWrites() const { return _logWrites; }

    // Resolves an abstract name to its row id, inserting a new row if absent.
    // Returns 0 if Initialize() has not yet run.
    uint64 ResolveAbstract(std::string_view name);

    // Public-API forwarders
    Value             Get(ActorRef a, ActorRef b, std::string_view key);
    bool              Has(ActorRef a, ActorRef b, std::string_view key);
    void              Set(ActorRef a, ActorRef b, std::string_view key, Value v);
    void              Clear(ActorRef a, ActorRef b, std::string_view key);
    std::vector<Edge> List(ActorRef a);
    std::vector<Edge> Query(QueryFilter const& f);
    void              Nuke(ActorRef a);
    size_t            EdgeCount();

private:
    StateGraphMgr() = default;

    // Phase 1 invariant: only Guild ↔ Abstract edges allowed for writes.
    // Reads accept any actor-type combination (returns default on miss).
    bool CheckWritePhase1(ActorRef const& a, ActorRef const& b,
                          char const* operation) const;

    // Ensures the per-actor row set is loaded from DB into the cache.
    // No-op if already loaded.
    void EnsureLoaded(ActorRef const& a);

    // Persistence helpers — implemented in StateGraphPersistence.cpp.
    void DbLoadAbstractActors();
    void DbLoadEdgesFor(ActorRef const& a);
    void DbWriteEdge(Edge const& e);
    void DbDeleteEdge(ActorRef const& a, ActorRef const& b, std::string const& key);
    void DbDeleteAllForActor(ActorRef const& a);

    // State (main-thread only — see file header)
    std::unordered_map<EdgeKey, Edge, EdgeKeyHash>           _edges;
    std::unordered_set<ActorKey, ActorKeyHash>               _loadedActors;
    std::unordered_map<std::string, uint64>                  _abstractByName;
    std::unordered_map<uint64, std::string>                  _abstractById;

    bool _initialized = false;
    bool _enabled     = true;
    bool _debug       = false;
    bool _logWrites   = false;
};

}  // namespace LivingWorld::StateGraph

#endif  // MOD_LIVING_WORLD_STATE_GRAPH_CACHE_H
