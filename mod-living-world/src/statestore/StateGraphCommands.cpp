#include "StateGraph.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

using namespace Acore::ChatCommands;
using namespace LivingWorld::StateGraph;

namespace
{

// --- Parsing helpers ---------------------------------------------

std::vector<std::string> Tokenize(char const* args)
{
    std::vector<std::string> out;
    if (!args)
        return out;
    std::istringstream iss(args);
    std::string tok;
    while (iss >> tok)
        out.push_back(std::move(tok));
    return out;
}

// Parse "guild:1" or "abstract:global" into an ActorRef.
// Returns true on success; sets out.type=None on failure.
bool ParseActor(std::string const& s, ActorRef& out, std::string& err)
{
    auto colon = s.find(':');
    if (colon == std::string::npos)
    {
        err = "expected '<type>:<id>' (e.g., 'guild:1', 'abstract:global')";
        return false;
    }
    std::string type = s.substr(0, colon);
    std::string id   = s.substr(colon + 1);
    std::transform(type.begin(), type.end(), type.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (type == "guild")
    {
        if (id.empty() || !std::all_of(id.begin(), id.end(),
                                       [](unsigned char c) { return std::isdigit(c); }))
        {
            err = "guild id must be a positive integer";
            return false;
        }
        out = ActorRef::Guild(static_cast<uint32>(std::stoul(id)));
        return true;
    }
    if (type == "player")
    {
        if (id.empty() || !std::all_of(id.begin(), id.end(),
                                       [](unsigned char c) { return std::isdigit(c); }))
        {
            err = "player id must be a positive integer (use the GUID counter)";
            return false;
        }
        out = ActorRef::Player(std::stoull(id));
        return true;
    }
    if (type == "abstract")
    {
        if (id.empty())
        {
            err = "abstract name cannot be empty";
            return false;
        }
        out = ActorRef::Abstract(id);
        if (!out.IsValid())
        {
            err = "abstract resolution failed (StateGraph not initialized?)";
            return false;
        }
        return true;
    }

    err = "unsupported actor type '" + type + "' (use guild, player, or abstract)";
    return false;
}

bool ParseValue(std::string const& typeStr, std::string const& valStr,
                Value& out, std::string& err)
{
    std::string t = typeStr;
    std::transform(t.begin(), t.end(), t.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (t == "int")
    {
        try { out = Value::OfInt(std::stoll(valStr)); return true; }
        catch (...) { err = "int value must be a signed integer"; return false; }
    }
    if (t == "bool")
    {
        std::string v = valStr;
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (v == "1" || v == "true"  || v == "yes" || v == "on")
        {
            out = Value::OfBool(true);
            return true;
        }
        if (v == "0" || v == "false" || v == "no"  || v == "off")
        {
            out = Value::OfBool(false);
            return true;
        }
        err = "bool value must be true|false|1|0|yes|no|on|off";
        return false;
    }
    if (t == "string")
    {
        out = Value::OfString(valStr);
        return true;
    }
    if (t == "enum")
    {
        out = Value::OfEnum(valStr);
        return true;
    }
    err = "unsupported value type '" + typeStr + "' (use int, bool, string, enum)";
    return false;
}

char const* ActorTypeShort(ActorType t)
{
    switch (t)
    {
        case ActorType::Guild:              return "guild";
        case ActorType::Player:             return "player";
        case ActorType::Faction:            return "faction";
        case ActorType::Abstract:           return "abstract";
        case ActorType::CreatureTemplate:   return "ctmpl";
        case ActorType::GameObjectTemplate: return "gotmpl";
        case ActorType::Location:           return "loc";
        case ActorType::CreatureSpawn:      return "cspawn";
        case ActorType::GameObjectSpawn:    return "gospawn";
        case ActorType::ItemInstance:       return "item";
        default:                            return "none";
    }
}

std::string FormatActor(ActorRef const& a)
{
    std::ostringstream os;
    os << ActorTypeShort(a.type) << ':' << a.id;
    return os.str();
}

std::string FormatValue(Value const& v)
{
    switch (v.type)
    {
        case ValueType::Int:    return "int:" + std::to_string(v.intVal);
        case ValueType::Bool:   return std::string("bool:") + (v.intVal ? "true" : "false");
        case ValueType::String: return "string:'" + v.stringVal + "'";
        case ValueType::Enum:   return "enum:" + v.stringVal;
    }
    return "?";
}

// --- Command handlers --------------------------------------------

bool HandleLwstateGet(ChatHandler* handler, char const* args)
{
    auto toks = Tokenize(args);
    if (toks.size() != 3)
    {
        handler->SendSysMessage("Usage: .lwstate get <actor_a> <actor_b> <key>");
        handler->SetSentErrorMessage(true);
        return false;
    }
    ActorRef a, b;
    std::string err;
    if (!ParseActor(toks[0], a, err))
    {
        handler->PSendSysMessage("actor_a: {}", err);
        handler->SetSentErrorMessage(true);
        return false;
    }
    if (!ParseActor(toks[1], b, err))
    {
        handler->PSendSysMessage("actor_b: {}", err);
        handler->SetSentErrorMessage(true);
        return false;
    }

    if (!Has(a, b, toks[2]))
    {
        handler->PSendSysMessage("(no edge for {} {} '{}')",
                                 FormatActor(a), FormatActor(b), toks[2]);
        return true;
    }
    Value v = Get(a, b, toks[2]);
    handler->PSendSysMessage("{} {} '{}' = {}",
                             FormatActor(a), FormatActor(b), toks[2], FormatValue(v));
    return true;
}

bool HandleLwstateHas(ChatHandler* handler, char const* args)
{
    auto toks = Tokenize(args);
    if (toks.size() != 3)
    {
        handler->SendSysMessage("Usage: .lwstate has <actor_a> <actor_b> <key>");
        handler->SetSentErrorMessage(true);
        return false;
    }
    ActorRef a, b;
    std::string err;
    if (!ParseActor(toks[0], a, err) || !ParseActor(toks[1], b, err))
    {
        handler->PSendSysMessage("{}", err);
        handler->SetSentErrorMessage(true);
        return false;
    }
    handler->PSendSysMessage("{}", Has(a, b, toks[2]) ? "true" : "false");
    return true;
}

bool HandleLwstateSet(ChatHandler* handler, char const* args)
{
    // Parse the first 4 whitespace-separated tokens (actor_a, actor_b,
    // key, type) by hand, then take the remainder of the line as the
    // value. This lets string and enum values contain spaces.
    // Surrounding double-quotes on the value are stripped.

    auto sendUsage = [handler]()
    {
        handler->SendSysMessage(
            "Usage: .lwstate set <actor_a> <actor_b> <key> <type> <value>");
        handler->SendSysMessage("  type: int | bool | string | enum");
        handler->SetSentErrorMessage(true);
    };

    if (!args)
    {
        sendUsage();
        return false;
    }

    char const* p = args;
    auto skipWs = [&p]()
    {
        while (*p == ' ' || *p == '\t')
            ++p;
    };
    auto readWord = [&p, &skipWs]() -> std::string
    {
        skipWs();
        char const* start = p;
        while (*p && *p != ' ' && *p != '\t')
            ++p;
        return std::string(start, p - start);
    };

    std::string aStr    = readWord();
    std::string bStr    = readWord();
    std::string keyStr  = readWord();
    std::string typeStr = readWord();
    skipWs();
    std::string valStr(p);

    // Trim trailing whitespace
    while (!valStr.empty() &&
           (valStr.back() == ' '  || valStr.back() == '\t' ||
            valStr.back() == '\n' || valStr.back() == '\r'))
        valStr.pop_back();

    // Strip surrounding double-quotes if present
    if (valStr.size() >= 2 && valStr.front() == '"' && valStr.back() == '"')
        valStr = valStr.substr(1, valStr.size() - 2);

    if (aStr.empty() || bStr.empty() || keyStr.empty() ||
        typeStr.empty() || valStr.empty())
    {
        sendUsage();
        return false;
    }

    ActorRef a, b;
    Value    v;
    std::string err;
    if (!ParseActor(aStr, a, err) || !ParseActor(bStr, b, err))
    {
        handler->PSendSysMessage("{}", err);
        handler->SetSentErrorMessage(true);
        return false;
    }
    if (!ParseValue(typeStr, valStr, v, err))
    {
        handler->PSendSysMessage("{}", err);
        handler->SetSentErrorMessage(true);
        return false;
    }

    Set(a, b, keyStr, std::move(v));
    handler->PSendSysMessage("Set {} {} '{}' = {}",
                             FormatActor(a), FormatActor(b), keyStr, valStr);
    return true;
}

bool HandleLwstateClear(ChatHandler* handler, char const* args)
{
    auto toks = Tokenize(args);
    if (toks.size() != 3)
    {
        handler->SendSysMessage("Usage: .lwstate clear <actor_a> <actor_b> <key>");
        handler->SetSentErrorMessage(true);
        return false;
    }
    ActorRef a, b;
    std::string err;
    if (!ParseActor(toks[0], a, err) || !ParseActor(toks[1], b, err))
    {
        handler->PSendSysMessage("{}", err);
        handler->SetSentErrorMessage(true);
        return false;
    }
    Clear(a, b, toks[2]);
    handler->PSendSysMessage("Cleared {} {} '{}'.",
                             FormatActor(a), FormatActor(b), toks[2]);
    return true;
}

bool HandleLwstateList(ChatHandler* handler, char const* args)
{
    auto toks = Tokenize(args);
    if (toks.size() != 1)
    {
        handler->SendSysMessage("Usage: .lwstate list <actor_a>");
        handler->SetSentErrorMessage(true);
        return false;
    }
    ActorRef a;
    std::string err;
    if (!ParseActor(toks[0], a, err))
    {
        handler->PSendSysMessage("{}", err);
        handler->SetSentErrorMessage(true);
        return false;
    }
    auto edges = List(a);
    if (edges.empty())
    {
        handler->PSendSysMessage("(no edges for {})", FormatActor(a));
        return true;
    }
    handler->PSendSysMessage("{} edges for {}:", edges.size(), FormatActor(a));
    for (auto const& e : edges)
    {
        handler->PSendSysMessage("  {} <-> {} '{}' = {}",
                                 FormatActor(e.a), FormatActor(e.b),
                                 e.key, FormatValue(e.value));
    }
    return true;
}

bool HandleLwstateNuke(ChatHandler* handler, char const* args)
{
    auto toks = Tokenize(args);
    if (toks.size() != 1)
    {
        handler->SendSysMessage("Usage: .lwstate nuke <actor_a>");
        handler->SetSentErrorMessage(true);
        return false;
    }
    ActorRef a;
    std::string err;
    if (!ParseActor(toks[0], a, err))
    {
        handler->PSendSysMessage("{}", err);
        handler->SetSentErrorMessage(true);
        return false;
    }
    Nuke(a);
    handler->PSendSysMessage("Nuked all edges for {}.", FormatActor(a));
    return true;
}

bool HandleLwstateCount(ChatHandler* handler, char const* /*args*/)
{
    handler->PSendSysMessage("StateGraph: {} cached edges.", EdgeCount());
    return true;
}

class LivingWorld_CommandScript : public CommandScript
{
public:
    LivingWorld_CommandScript() : CommandScript("LivingWorld_CommandScript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable lwstateSub =
        {
            { "get",   HandleLwstateGet,   SEC_GAMEMASTER, Console::Yes },
            { "has",   HandleLwstateHas,   SEC_GAMEMASTER, Console::Yes },
            { "set",   HandleLwstateSet,   SEC_GAMEMASTER, Console::Yes },
            { "clear", HandleLwstateClear, SEC_GAMEMASTER, Console::Yes },
            { "list",  HandleLwstateList,  SEC_GAMEMASTER, Console::Yes },
            { "nuke",  HandleLwstateNuke,  SEC_GAMEMASTER, Console::Yes },
            { "count", HandleLwstateCount, SEC_GAMEMASTER, Console::Yes },
        };
        static ChatCommandTable root =
        {
            { "lwstate", lwstateSub },
        };
        return root;
    }
};

}  // namespace

void AddLivingWorldCommandScripts()
{
    new LivingWorld_CommandScript();
}
