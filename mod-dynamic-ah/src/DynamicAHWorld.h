#pragma once

#include "DynamicAHTypes.h"
#include "ScriptMgr.h"
#include "World.h"
#include "Config.h"
class ChatHandler;

namespace ModDynamicAH
{
    // Thin world script that delegates lifecycle to Service
    class DynamicAHWorld : public WorldScript
    {
    public:
        DynamicAHWorld();
        static bool HandlePlan(ChatHandler* handler);
        static bool HandleRun(ChatHandler* handler);
        static bool HandleLoop(ChatHandler* handler);
        static bool HandleDryRun(ChatHandler* handler);
        static bool HandleStatus(ChatHandler* handler);
        void OnAfterConfigLoad(bool /*reload*/) override;
        void OnUpdate(uint32 diff) override;

    private:
        static uint64 NowMs();
    };
} // namespace ModDynamicAH
