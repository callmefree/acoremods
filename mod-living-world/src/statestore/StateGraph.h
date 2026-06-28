#ifndef MOD_LIVING_WORLD_STATE_GRAPH_H
#define MOD_LIVING_WORLD_STATE_GRAPH_H

#include "Define.h"

#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Public API for the Living World relationship state graph. See
// docs/living_world_state_graph_design.md for the full design.
//
// THREADING: main worldserver thread only. The implementation is
// lock-free and assumes single-threaded access. AC's ScriptMgr hooks
// (WorldScript, PlayerScript, CreatureScript, chat commands) all fire
// on the main tick, so any caller from a script context is fine.
// Calling from a custom thread is a programming error.
//
// Phase 1 supported Guild and Abstract only.
// Phase 2 adds Player as a writable actor type and multi-actor write
// (SetMulti) and any-match read (HasAny) helpers, so events can be
// recorded against an actor's full membership tree (player + guild,
// future: party + raid) and NPCs can OR-check across that tree.
// Faction, Templates, Spawns, Items remain declared-but-not-implemented.

namespace LivingWorld::StateGraph
{

enum class ActorType : uint8
{
    None               = 0,
    Guild              = 1,
    Player             = 2,    // Phase 2 — not implemented
    Faction            = 3,    // Phase 2 — not implemented
    Abstract           = 4,
    CreatureTemplate   = 10,   // Phase 3 — not implemented
    GameObjectTemplate = 11,   // Phase 3 — not implemented
    Location           = 12,   // Phase 3 — not implemented
    CreatureSpawn      = 20,   // Phase 4 — not implemented
    GameObjectSpawn    = 21,   // Phase 4 — not implemented
    ItemInstance       = 22,   // Phase 4 — not implemented
};

enum class ValueType : uint8
{
    Int    = 1,
    Bool   = 2,
    String = 3,
    Enum   = 4,
};

struct ActorRef
{
    ActorType type = ActorType::None;
    uint64    id   = 0;

    bool IsValid() const { return type != ActorType::None; }
    bool operator==(ActorRef const& o) const { return type == o.type && id == o.id; }
    bool operator!=(ActorRef const& o) const { return !(*this == o); }

    // Phase 1 helpers
    static ActorRef Guild(uint32 guildId);

    // Resolves `name` against mod_living_world_abstract_actors,
    // creating a row on first reference. Returns ActorType::None
    // if Initialize() has not been called.
    static ActorRef Abstract(std::string_view name);

    // Phase 2 helper. `guidLow` is the player GUID's counter
    // (`Player::GetGUID().GetCounter()`); kept as a primitive so this
    // header stays independent of AC's ObjectGuid.
    static ActorRef Player(uint64 guidLow);
};

struct Value
{
    ValueType   type   = ValueType::Int;
    int64       intVal = 0;
    std::string stringVal;

    static Value OfInt(int64 v);
    static Value OfBool(bool v);
    static Value OfString(std::string s);
    static Value OfEnum(std::string s);

    bool        AsBool()   const;
    int64       AsInt()    const;
    std::string AsString() const;
};

struct Edge
{
    ActorRef    a;
    ActorRef    b;
    std::string key;
    Value       value;
    time_t      updatedAt = 0;
};

struct QueryFilter
{
    std::optional<ActorRef>    actorA;
    std::optional<ActorRef>    actorB;
    std::optional<std::string> key;
};

// Read
Value Get(ActorRef a, ActorRef b, std::string_view key);
bool  Has(ActorRef a, ActorRef b, std::string_view key);

// Write
void  Set(ActorRef a, ActorRef b, std::string_view key, Value v);
void  Clear(ActorRef a, ActorRef b, std::string_view key);

// Phase 2: membership-tree helpers.
//
// SetMulti writes the same edge under each actor in `actors`. Use it
// when an event should be recorded against a player AND each group
// container they belong to at trigger time.
//
// HasAny returns true if any of `actors` has the edge — the OR-across-
// memberships rule: if your guild remembers, or your party remembers,
// or you remember, the NPC reacts.
void SetMulti(std::vector<ActorRef> const& actors, ActorRef b,
              std::string_view key, Value v);
bool HasAny(std::vector<ActorRef> const& actors, ActorRef b,
            std::string_view key);

// Bulk read
std::vector<Edge> List(ActorRef a);
std::vector<Edge> Query(QueryFilter const& f);

// Admin
void   Nuke(ActorRef a);   // remove all edges where actor appears as a or b
size_t EdgeCount();

// Lifecycle (called by the module's WorldScript)
void Initialize();
void Shutdown();

}  // namespace LivingWorld::StateGraph

#endif  // MOD_LIVING_WORLD_STATE_GRAPH_H
