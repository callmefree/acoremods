#include "StateGraphCache.h"

#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "QueryResult.h"

#include <ctime>

namespace LivingWorld::StateGraph
{

namespace
{
    // SQL-quote escape for our format-string call sites. Doubles every
    // single quote per the SQL standard. Sufficient for Phase 1's
    // controlled inputs; slice 2 may refactor to PreparedStatements.
    std::string SqlEscape(std::string const& s)
    {
        std::string out;
        out.reserve(s.size());
        for (char c : s)
        {
            if (c == '\'')
                out += "''";
            else
                out += c;
        }
        return out;
    }
}

// Free function used by ResolveAbstract. Defined at namespace scope so
// the cache implementation can call it without exposing a per-row
// insert method on StateGraphMgr.
uint64 DbInsertAbstractActor(std::string const& name)
{
    std::string esc = SqlEscape(name);

    // INSERT IGNORE so a concurrent caller that already inserted the same
    // name doesn't fail. Then SELECT id back.
    CharacterDatabase.Execute(
        "INSERT IGNORE INTO mod_living_world_abstract_actors (name) "
        "VALUES ('{}')",
        esc);

    QueryResult result = CharacterDatabase.Query(
        "SELECT id FROM mod_living_world_abstract_actors WHERE name = '{}'",
        esc);

    if (!result)
    {
        LOG_ERROR("module",
                  "mod-living-world: failed to read back abstract actor "
                  "id for name '{}'.", name);
        return 0;
    }

    return result->Fetch()[0].Get<uint64>();
}

void StateGraphMgr::DbLoadAbstractActors()
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT id, name FROM mod_living_world_abstract_actors");

    if (!result)
    {
        LOG_WARN("module",
                 "mod-living-world: mod_living_world_abstract_actors is "
                 "empty or missing. Phase 1 migrations must be applied "
                 "before the module is useful.");
        return;
    }

    do
    {
        Field* f      = result->Fetch();
        uint64 id     = f[0].Get<uint64>();
        std::string n = f[1].Get<std::string>();
        _abstractByName.emplace(n, id);
        _abstractById.emplace(id, std::move(n));
    } while (result->NextRow());

    LOG_INFO("module",
             "mod-living-world: loaded {} abstract actor rows.",
             _abstractByName.size());
}

void StateGraphMgr::DbLoadEdgesFor(ActorRef const& a)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT actor_a_type, actor_a_id, actor_b_type, actor_b_id, "
        "       state_key, value_type, value_int, value_string, "
        "       UNIX_TIMESTAMP(updated_at) "
        "FROM mod_living_world_relationship_state "
        "WHERE (actor_a_type = {} AND actor_a_id = {}) "
        "   OR (actor_b_type = {} AND actor_b_id = {})",
        static_cast<uint32>(a.type), a.id,
        static_cast<uint32>(a.type), a.id);

    if (!result)
        return;

    size_t loaded = 0;
    do
    {
        Field* f = result->Fetch();
        Edge e;
        e.a.type      = static_cast<ActorType>(f[0].Get<uint8>());
        e.a.id        = f[1].Get<uint64>();
        e.b.type      = static_cast<ActorType>(f[2].Get<uint8>());
        e.b.id        = f[3].Get<uint64>();
        e.key         = f[4].Get<std::string>();
        e.value.type  = static_cast<ValueType>(f[5].Get<uint8>());

        if (e.value.type == ValueType::Int || e.value.type == ValueType::Bool)
            e.value.intVal = f[6].Get<int64>();
        else
            e.value.stringVal = f[7].Get<std::string>();

        e.updatedAt = static_cast<time_t>(f[8].Get<int64>());

        EdgeKey ek{e.a.type, e.a.id, e.b.type, e.b.id, e.key};
        _edges[ek] = std::move(e);
        ++loaded;
    } while (result->NextRow());

    if (_debug)
        LOG_INFO("module",
                 "mod-living-world: loaded {} edges for actor "
                 "(type={}, id={}).",
                 loaded, static_cast<uint32>(a.type), a.id);
}

void StateGraphMgr::DbWriteEdge(Edge const& e)
{
    bool useString = (e.value.type == ValueType::String ||
                      e.value.type == ValueType::Enum);
    std::string keyEsc = SqlEscape(e.key);

    if (useString)
    {
        CharacterDatabase.Execute(
            "INSERT INTO mod_living_world_relationship_state "
            "(actor_a_type, actor_a_id, actor_b_type, actor_b_id, "
            " state_key, value_type, value_int, value_string) "
            "VALUES ({}, {}, {}, {}, '{}', {}, NULL, '{}') "
            "ON DUPLICATE KEY UPDATE "
            " value_type = VALUES(value_type), "
            " value_int = VALUES(value_int), "
            " value_string = VALUES(value_string)",
            static_cast<uint32>(e.a.type), e.a.id,
            static_cast<uint32>(e.b.type), e.b.id,
            keyEsc, static_cast<uint32>(e.value.type),
            SqlEscape(e.value.stringVal));
    }
    else
    {
        CharacterDatabase.Execute(
            "INSERT INTO mod_living_world_relationship_state "
            "(actor_a_type, actor_a_id, actor_b_type, actor_b_id, "
            " state_key, value_type, value_int, value_string) "
            "VALUES ({}, {}, {}, {}, '{}', {}, {}, NULL) "
            "ON DUPLICATE KEY UPDATE "
            " value_type = VALUES(value_type), "
            " value_int = VALUES(value_int), "
            " value_string = VALUES(value_string)",
            static_cast<uint32>(e.a.type), e.a.id,
            static_cast<uint32>(e.b.type), e.b.id,
            keyEsc, static_cast<uint32>(e.value.type),
            e.value.intVal);
    }
}

void StateGraphMgr::DbDeleteEdge(ActorRef const& a, ActorRef const& b,
                                 std::string const& key)
{
    CharacterDatabase.Execute(
        "DELETE FROM mod_living_world_relationship_state "
        "WHERE actor_a_type = {} AND actor_a_id = {} "
        "  AND actor_b_type = {} AND actor_b_id = {} "
        "  AND state_key = '{}'",
        static_cast<uint32>(a.type), a.id,
        static_cast<uint32>(b.type), b.id,
        SqlEscape(key));
}

void StateGraphMgr::DbDeleteAllForActor(ActorRef const& a)
{
    CharacterDatabase.Execute(
        "DELETE FROM mod_living_world_relationship_state "
        "WHERE (actor_a_type = {} AND actor_a_id = {}) "
        "   OR (actor_b_type = {} AND actor_b_id = {})",
        static_cast<uint32>(a.type), a.id,
        static_cast<uint32>(a.type), a.id);
}

}  // namespace LivingWorld::StateGraph
