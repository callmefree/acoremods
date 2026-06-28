#pragma once

#include "ScriptMgr.h"
#include "ChatCommand.h"

class DynamicAHCommands final : public CommandScript {
public:
    DynamicAHCommands();
    Acore::ChatCommands::ChatCommandTable GetCommands() const override;
    static bool HandleInterval(ChatHandler *handler, Optional<uint32> minutesOpt);
    static bool HandleQueue(ChatHandler *handler);
    static bool HandleClear(ChatHandler *handler);
    static bool HandlePriceCmd(ChatHandler *handler, Optional<std::string> catOpt, Optional<uint32> pctOpt);
    static bool HandleSetup(ChatHandler *handler);
    static bool HandleBuyShow(ChatHandler *handler);
    static bool HandleBuyEnable(ChatHandler *handler, Optional<uint32> onOff);
    static bool HandleBuyBudget(ChatHandler *handler, Optional<uint32> goldOpt);
    static bool HandleBuyMargin(ChatHandler *handler, Optional<uint32> pctOpt);
    static bool HandleBuyPerItem(ChatHandler *handler, Optional<uint32> capOpt);
    static bool HandleBuyOnce(ChatHandler *handler);
    static bool HandleBuyFund(ChatHandler *handler, Optional<uint32> goldOpt, Optional<std::string> whichOpt);
    static bool HandleCapsShow(ChatHandler *handler);
    static bool HandleCapsEnable(ChatHandler *handler, Optional<uint32> onOff);
    static bool HandleCapsResetCounts(ChatHandler *handler);
    static bool HandleCapsDefaults(ChatHandler *handler);
    static bool HandleCapsSetTotal(ChatHandler *handler, uint32 value);
    static bool HandleCapsSetHouse(ChatHandler *handler, std::string which, uint32 value);
    static bool HandleCapsSetFamily(ChatHandler *handler, std::string famName, uint32 value);
    static bool HandleContext(ChatHandler *handler, Optional<std::string> keyOpt, Optional<uint32> valOpt);
};
