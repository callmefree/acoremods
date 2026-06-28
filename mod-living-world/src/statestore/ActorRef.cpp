#include "StateGraph.h"
#include "StateGraphCache.h"

namespace LivingWorld::StateGraph
{

ActorRef ActorRef::Guild(uint32 guildId)
{
    ActorRef ref;
    ref.type = ActorType::Guild;
    ref.id   = static_cast<uint64>(guildId);
    return ref;
}

ActorRef ActorRef::Abstract(std::string_view name)
{
    uint64 id = StateGraphMgr::Instance().ResolveAbstract(name);
    if (id == 0)
        return ActorRef{};  // type=None, id=0
    ActorRef ref;
    ref.type = ActorType::Abstract;
    ref.id   = id;
    return ref;
}

ActorRef ActorRef::Player(uint64 guidLow)
{
    ActorRef ref;
    ref.type = ActorType::Player;
    ref.id   = guidLow;
    return ref;
}

// --- Value helpers ------------------------------------------------

Value Value::OfInt(int64 v)
{
    Value out;
    out.type   = ValueType::Int;
    out.intVal = v;
    return out;
}

Value Value::OfBool(bool v)
{
    Value out;
    out.type   = ValueType::Bool;
    out.intVal = v ? 1 : 0;
    return out;
}

Value Value::OfString(std::string s)
{
    Value out;
    out.type      = ValueType::String;
    out.stringVal = std::move(s);
    return out;
}

Value Value::OfEnum(std::string s)
{
    Value out;
    out.type      = ValueType::Enum;
    out.stringVal = std::move(s);
    return out;
}

bool Value::AsBool() const
{
    if (type == ValueType::Bool || type == ValueType::Int)
        return intVal != 0;
    if (type == ValueType::String || type == ValueType::Enum)
        return !stringVal.empty();
    return false;
}

int64 Value::AsInt() const
{
    if (type == ValueType::Int || type == ValueType::Bool)
        return intVal;
    return 0;
}

std::string Value::AsString() const
{
    if (type == ValueType::String || type == ValueType::Enum)
        return stringVal;
    if (type == ValueType::Bool)
        return intVal ? "true" : "false";
    if (type == ValueType::Int)
        return std::to_string(intVal);
    return std::string{};
}

}  // namespace LivingWorld::StateGraph
