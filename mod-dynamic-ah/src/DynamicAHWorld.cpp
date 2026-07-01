#include "DynamicAHWorld.h"
#include "DynamicAHPlanner.h"
#include "ModDynamicAHService.h"
#include "GameTime.h"
#include "Chat.h"

using namespace ModDynamicAH;

static inline uint64 NowMsInternal() { return (uint64)GameTime::GetGameTimeMS().count(); }

DynamicAHWorld::DynamicAHWorld() : WorldScript("DynamicAHWorld") {}

void DynamicAHWorld::OnAfterConfigLoad(bool /*reload*/)
{
    Service::Instance().OnConfigLoad();
}

void DynamicAHWorld::OnUpdate(uint32 diff)
{
    Service::Instance().OnUpdate(diff);
}

uint64 DynamicAHWorld::NowMs()
{
    return NowMsInternal();
}

bool DynamicAHWorld::HandlePlan(ChatHandler *handler)
{
    Service::Instance().PlanOnce(handler);
    return true;
}

bool DynamicAHWorld::HandleRun(ChatHandler *handler)
{
    Service::Instance().ApplyOnce(handler);
    return true;
}

bool DynamicAHWorld::HandleLoop(ChatHandler *handler)
{
    auto &g = Service::Instance().State();
    g.loopEnabled = !g.loopEnabled;
    if (handler)
        handler->PSendSysMessage("动态AH: 循环已{}", g.loopEnabled ? "ON" : "OFF");
    return true;
}

bool DynamicAHWorld::HandleDryRun(ChatHandler *handler)
{
    auto &g = Service::Instance().State();
    g.dryRun = !g.dryRun;
    if (handler)
        handler->PSendSysMessage("动态AH: 模拟模式已{}", g.dryRun ? "ON" : "OFF");
    return true;
}

bool DynamicAHWorld::HandleStatus(ChatHandler *handler)
{
    auto &s = Service::Instance().State();
    if (handler)
    {
        handler->PSendSysMessage("动态AH: 循环={} 模拟={} 间隔={}分 下次运行={} 排队上架={} 排队购买={}",
                                 s.loopEnabled ? "ON" : "OFF",
                                 s.dryRun ? "ON" : "OFF",
                                 s.intervalMin,
                                 (unsigned long long)s.nextRunMs,
                                 s.postQueue.Size(),
                                 Service::Instance().Buy().QueueSize());
    }
    return true;
}
