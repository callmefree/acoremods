#include "DynamicAHCommands.h"

#include "DynamicAHWorld.h"
#include "DynamicAHTypes.h"
#include "ModDynamicAHService.h"
#include "ModDynamicAHBuy.h"
#include "DynamicAHSetup.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "Optional.h"
#include "World.h"
#include "GameTime.h"

#include <algorithm>
#include <string>

using ::Optional;
using Acore::ChatCommands::ChatCommandTable;

// -----------------------------------------------------------------------
// CommandScript
// -----------------------------------------------------------------------
DynamicAHCommands::DynamicAHCommands() : CommandScript("DynamicAHCommands") {}

ChatCommandTable DynamicAHCommands::GetCommands() const
{
    using namespace ModDynamicAH; // only in this small scope

    static ChatCommandTable buySub =
        {
            {"", HandleBuyShow, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
            {"enable", HandleBuyEnable, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
            {"budget", HandleBuyBudget, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
            {"margin", HandleBuyMargin, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
            {"peritem", HandleBuyPerItem, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
            {"once", HandleBuyOnce, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
            {"fund", HandleBuyFund, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
        };

    static ChatCommandTable capsSetSub =
        {
            {"total", HandleCapsSetTotal, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
            {"house", HandleCapsSetHouse, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
            {"family", HandleCapsSetFamily, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
        };

    static ChatCommandTable capsSub =
        {
            {"", HandleCapsShow, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
            {"show", HandleCapsShow, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
            {"enable", HandleCapsEnable, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
            {"set", capsSetSub},
            {"resetcounts", HandleCapsResetCounts, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
            {"defaults", HandleCapsDefaults, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
        };

    static ChatCommandTable rootSub =
        {
            // world+loop
            {"plan", DynamicAHWorld::HandlePlan, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
            {"run", DynamicAHWorld::HandleRun, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
            {"loop", DynamicAHWorld::HandleLoop, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
            {"dryrun", DynamicAHWorld::HandleDryRun, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
            {"interval", HandleInterval, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
            {"status", DynamicAHWorld::HandleStatus, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},

            // queue + maintenance
            {"queue", HandleQueue, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
            {"clear", HandleClear, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},

            // pricing tweaks
            {"price", HandlePriceCmd, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},

            // setup (sellers / account)
            {"setup", HandleSetup, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},

            // buy engine
            {"buy", buySub},

            // caps
            {"caps", capsSub},

            // context controls
            {"context", HandleContext, SEC_ADMINISTRATOR, Acore::ChatCommands::Console::Yes},
        };

    static ChatCommandTable table =
        {
            {"dah", rootSub}};

    return table;
}

// -----------------------------------------------------------------------
// Handlers (thin wrappers)
// -----------------------------------------------------------------------

// interval (minutes)
bool DynamicAHCommands::HandleInterval(ChatHandler *handler, Optional<uint32> minutesOpt)
{
    auto &st = ModDynamicAH::Service::Instance().State();
    if (!minutesOpt)
    {
        handler->PSendSysMessage("Usage: .dah interval <minutes> (current: {} m)", st.intervalMin);
        return true;
    }
    st.intervalMin = *minutesOpt ? *minutesOpt : 1;
    st.nextRunMs = uint64(GameTime::GetGameTimeMS().count()) + 1000;
    handler->PSendSysMessage("ModDynamicAH: interval set to {} minutes", st.intervalMin);
    return true;
}

bool DynamicAHCommands::HandleQueue(ChatHandler *handler)
{
    auto &svc = ModDynamicAH::Service::Instance();
    auto &st = svc.State();

    handler->PSendSysMessage("ModDynamicAH: postQueue={} buyQueue={} budgetUsed={}/{}",
                             st.postQueue.Size(),
                             uint32(svc.Buy().QueueSize()),
                             uint32(svc.Buy().BudgetUsed()),
                             uint32(svc.Buy().BudgetLimit()));
    return true;
}

bool DynamicAHCommands::HandleClear(ChatHandler *handler)
{
    auto &svc = ModDynamicAH::Service::Instance();
    auto &st = svc.State();

    // apply everything pending (posts + buys)
    ModDynamicAH::DynamicAHPosting::ApplyPlanOnWorld(st, /*maxToApply=*/1000000, handler);
    svc.Buy().Apply(1000000, st.dryRun, handler);

    handler->PSendSysMessage("ModDynamicAH: applied/cleared pending posts & buys (dry-run={}): postQ={} buyQ={}",
                             st.dryRun ? 1u : 0u,
                             st.postQueue.Size(),
                             uint32(svc.Buy().QueueSize()));
    return true;
}

// price <dust|essence|shard|elemental|rareraw> <percent>
bool DynamicAHCommands::HandlePriceCmd(ChatHandler *handler, Optional<std::string> catOpt, Optional<uint32> pctOpt)
{
    auto &st = ModDynamicAH::Service::Instance().State();

    if (!catOpt)
    {
        handler->PSendSysMessage("Price multipliers (percent): dust={} essence={} shard={} elemental={} rareRaw={}",
                                 int(st.mulDust * 100), int(st.mulEssence * 100), int(st.mulShard * 100),
                                 int(st.mulElemental * 100), int(st.mulRareRaw * 100));
        handler->PSendSysMessage("Usage: .dah price <dust|essence|shard|elemental|rareraw> <percent>");
        return true;
    }
    if (!pctOpt)
    {
        handler->PSendSysMessage("Missing percent value.");
        return true;
    }

    double v = std::max(10.0, std::min<double>(*pctOpt, 1000.0)) / 100.0;
    std::string c = *catOpt;
    std::transform(c.begin(), c.end(), c.begin(), ::tolower);

    if (c == "dust")
        st.mulDust = v;
    else if (c == "essence")
        st.mulEssence = v;
    else if (c == "shard")
        st.mulShard = v;
    else if (c == "elemental")
        st.mulElemental = v;
    else if (c == "rareraw")
        st.mulRareRaw = v;
    else
    {
        handler->PSendSysMessage("Unknown category.");
        return true;
    }

    handler->PSendSysMessage("Multiplier for {} set to {:.2f} ({}%%)", c.c_str(), v, int(v * 100));
    return true;
}

// setup (creates seller account + 3 seller chars if missing; echo GUIDs)
bool DynamicAHCommands::HandleSetup(ChatHandler *handler)
{
    auto &s = ModDynamicAH::Service::Instance().State();
    ModDynamicAH::DynamicAHSetup::RunSetup(s, handler);
    return true;
}

// ---- BUY subcommands ---------------------------------------------------
bool DynamicAHCommands::HandleBuyShow(ChatHandler *handler)
{
    ModDynamicAH::Service::Instance().Buy().CmdShow(handler);
    return true;
}

bool DynamicAHCommands::HandleBuyEnable(ChatHandler *handler, Optional<uint32> onOff)
{
    if (!onOff)
    {
        handler->PSendSysMessage("Usage: .dah buy enable <0|1>");
        return true;
    }
    ModDynamicAH::Service::Instance().Buy().CmdEnable(handler, (*onOff != 0));
    return true;
}

bool DynamicAHCommands::HandleBuyBudget(ChatHandler *handler, Optional<uint32> goldOpt)
{
    if (!goldOpt)
    {
        ModDynamicAH::Service::Instance().Buy().CmdShow(handler);
        handler->PSendSysMessage("Usage: .dah buy budget <gold>");
        return true;
    }
    ModDynamicAH::Service::Instance().Buy().CmdBudget(handler, *goldOpt);
    return true;
}

bool DynamicAHCommands::HandleBuyMargin(ChatHandler *handler, Optional<uint32> pctOpt)
{
    if (!pctOpt)
    {
        ModDynamicAH::Service::Instance().Buy().CmdShow(handler);
        handler->PSendSysMessage("Usage: .dah buy margin <percent>");
        return true;
    }
    ModDynamicAH::Service::Instance().Buy().CmdMargin(handler, *pctOpt);
    return true;
}

bool DynamicAHCommands::HandleBuyPerItem(ChatHandler *handler, Optional<uint32> capOpt)
{
    if (!capOpt)
    {
        ModDynamicAH::Service::Instance().Buy().CmdShow(handler);
        handler->PSendSysMessage("Usage: .dah buy peritem <N>");
        return true;
    }
    ModDynamicAH::Service::Instance().Buy().CmdPerItem(handler, *capOpt);
    return true;
}

bool DynamicAHCommands::HandleBuyOnce(ChatHandler *handler)
{
    auto &svc = ModDynamicAH::Service::Instance();
    // Do exactly one buy operation using the current dry-run setting
    svc.Buy().Apply(1, svc.State().dryRun, handler);
    return true;
}

// .dah buy fund <gold> [alliance|horde|neutral|all]
bool DynamicAHCommands::HandleBuyFund(ChatHandler *handler, Optional<uint32> goldOpt, Optional<std::string> whichOpt)
{
    if (!goldOpt)
    {
        handler->PSendSysMessage("Usage: .dah buy fund <gold> [alliance|horde|neutral|all]");
        return true;
    }
    ModDynamicAH::Service::Instance().CmdFund(handler, *goldOpt, whichOpt ? *whichOpt : "all");
    return true;
}

// ---- CAPS --------------------------------------------------------------
bool DynamicAHCommands::HandleCapsShow(ChatHandler *handler)
{
    ModDynamicAH::Service::Instance().CmdCapsShow(handler);
    return true;
}

bool DynamicAHCommands::HandleCapsEnable(ChatHandler *handler, Optional<uint32> onOff)
{
    if (!onOff)
    {
        handler->PSendSysMessage("Usage: .dah caps enable <0|1>");
        return true;
    }
    ModDynamicAH::Service::Instance().CmdCapsEnable(handler, (*onOff != 0));
    return true;
}

bool DynamicAHCommands::HandleCapsResetCounts(ChatHandler *handler)
{
    ModDynamicAH::Service::Instance().CmdCapsReset(handler);
    return true;
}

bool DynamicAHCommands::HandleCapsDefaults(ChatHandler *handler)
{
    ModDynamicAH::Service::Instance().CmdCapsDefaults(handler);
    return true;
}

bool DynamicAHCommands::HandleCapsSetTotal(ChatHandler *handler, uint32 value)
{
    ModDynamicAH::Service::Instance().CmdCapsSetTotal(handler, value);
    return true;
}

bool DynamicAHCommands::HandleCapsSetHouse(ChatHandler *handler, std::string which, uint32 value)
{
    ModDynamicAH::Service::Instance().CmdCapsSetHouse(handler, which, value);
    return true;
}

bool DynamicAHCommands::HandleCapsSetFamily(ChatHandler *handler, std::string famName, uint32 value)
{
    ModDynamicAH::Service::Instance().CmdCapsSetFamily(handler, famName, value);
    return true;
}

// ---- Context tuning ----------------------------------------------------
// .dah context -> show
// .dah context maxperbracket <N>
// .dah context weightboost <percent>
// .dah context skipvendor <0|1>
// .dah context debug <0|1>
bool DynamicAHCommands::HandleContext(ChatHandler *handler, Optional<std::string> keyOpt, Optional<uint32> valOpt)
{
    return ModDynamicAH::Service::Instance().CmdContext(handler, keyOpt, valOpt);
}
