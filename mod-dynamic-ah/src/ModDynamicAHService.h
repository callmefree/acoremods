#pragma once

#include "ModDynamicAHBuy.h"
#include "DynamicAHPlanner.h"
#include "DynamicAHPosting.h"
#include "DynamicAHState.h"
#include "DynamicAHPosting.h"

class ChatHandler;

namespace ModDynamicAH
{
    class Service
    {
    public:
        static Service &Instance();

        // lifecycle
        void OnConfigLoad();
        void OnUpdate(uint32_t /*diff*/);

        // admin operations
        void PlanOnce(ChatHandler *handler);
        void ApplyOnce(ChatHandler *handler);
        void ClearQueues(ChatHandler *handler);
        void ShowQueue(ChatHandler *handler);
        void ToggleLoop(bool enable, ChatHandler *handler);
        void SetDryRun(bool dry, ChatHandler *handler);
        void SetInterval(uint32_t minutes, ChatHandler *handler);
        void ShowStatus(ChatHandler *handler);

        // buy engine passthrough
        BuyEngine &Buy() { return buy_; }
        ModuleState &State() { return state_; }
        DynamicAHPlanner &Planner() { return planner_; }
        DynamicAHPlanner const &Planner() const { return planner_; }
        void CmdFund(ChatHandler *handler, uint32 gold, std::string const &which);
        void CmdCapsShow(ChatHandler *handler);
        void CmdCapsEnable(ChatHandler* handler, bool on);
        void CmdCapsSetFamily(ChatHandler* handler, std::string famName, uint32 value);
        bool CmdContext(ChatHandler* handler, Optional<std::string> keyOpt, Optional<uint32> valOpt);

        void CmdCapsSetHouse(ChatHandler* handler, std::string which, uint32 value);
        void CmdCapsSetTotal(ChatHandler* handler, uint32 value);
        void CmdCapsDefaults(ChatHandler* handler);
        void CmdCapsReset(ChatHandler* handler);

    private:
        Service() = default;

        void DoOneCycle();

        ModuleState state_;
        ModDynamicAH::DynamicAHPlanner planner_;
        BuyEngine buy_;
    };
}
