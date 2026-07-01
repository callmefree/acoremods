// Innkeeper gossip surface for mod-terror-zones. Mirrors the read-only
// `.zones` / `.zones next` / `.zones history` commands into an innkeeper
// gossip menu so regular players (not just GMs) can read what's terrorized,
// when the next rotation lands, recent history, and how the system works.
//
// Coexistence: this uses the additive `OnCreatureGossipHelloAppend` hook
// (acoremods core patch) so it stacks alongside other modules' innkeeper
// options (e.g. mod-bag-sorter) instead of first-wins clobbering. The core
// prepares + sends the native menu; we only AddGossipItemFor.

#include "TerrorZonesMgr.h"

#include "Chat.h"
#include "Creature.h"
#include "GossipDef.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

using namespace mod_terror_zones;

namespace
{
    // Distinctive sender so our selections never collide with the
    // innkeeper's native DB options (sender 0) or other modules.
    enum TerrorZonesGossip : uint32
    {
        SENDER_TZ          = 0x7202,   // "TZ"

        ACTION_TZ_OPEN     = 7200,
        ACTION_TZ_INFO     = 7201,
        ACTION_TZ_NEXT     = 7202,
        ACTION_TZ_HISTORY  = 7203,
        ACTION_TZ_HOW      = 7204,
        ACTION_TZ_BACK     = 7205,
        ACTION_TZ_CLOSE    = 7206,
    };

    std::string FormatRemaining(uint32 secs)
    {
        if (secs >= 3600)
        {
            uint32 h = secs / 3600;
            uint32 m = (secs % 3600) / 60;
            if (m == 0)
                return (h == 1) ? "1 小时" : std::to_string(h) + " 小时";
            return std::to_string(h) + "时" + std::to_string(m) + "分";
        }
        if (secs >= 60)
        {
            uint32 m = secs / 60;
            return (m == 1) ? "1 分钟" : std::to_string(m) + " 分钟";
        }
        return std::to_string(secs) + "秒";
    }

    std::string RelativeAge(uint64 tickAt, uint64 now)
    {
        if (tickAt > now)
            return "刚刚";
        uint64 delta = now - tickAt;
        if (delta < 60)
            return std::to_string(delta) + "秒前";
        if (delta < 3600)
            return std::to_string(delta / 60) + "分前";
        if (delta < 86400)
            return std::to_string(delta / 3600) + "时前";
        return std::to_string(delta / 86400) + "天前";
    }

    std::string FormatEventRemaining(uint64 now, uint64 startsAt, uint64 endsAt)
    {
        if (now < startsAt)
        {
            uint64 delta = startsAt - now;
            if (delta < 60)
                return "将在" + std::to_string(delta) + "秒后出现";
            return "将在" + std::to_string(delta / 60) + "分后出现";
        }
        if (now >= endsAt)
            return "即将离开";
        uint64 delta = endsAt - now;
        if (delta < 60)
            return "剩余" + std::to_string(delta) + "秒";
        return "剩余" + std::to_string(delta / 60) + "分";
    }

    // Re-open the Terror Zones submenu so the player can keep reading
    // after an info action prints to chat.
    void SendTzMenu(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "哪些区域被恐怖化了？", SENDER_TZ, ACTION_TZ_INFO);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "下次轮换什么时候？", SENDER_TZ, ACTION_TZ_NEXT);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "最近轮换记录", SENDER_TZ, ACTION_TZ_HISTORY);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "恐怖地带怎么玩？", SENDER_TZ, ACTION_TZ_HOW);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "没什么", SENDER_TZ, ACTION_TZ_CLOSE);
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
    }

    void PrintInfo(Player* player)
    {
        auto& mgr = TerrorZonesMgr::Instance();
        ChatHandler ch(player->GetSession());
        if (!mgr.IsEnabled())
        {
            ch.SendSysMessage("恐怖地带功能已禁用。");
            return;
        }
        ActiveRotation rot = mgr.GetActiveRotation();
        if (rot.slots.empty())
        {
            ch.SendSysMessage("当前没有被恐怖化的区域。请等待下次轮换。");
            return;
        }
        uint32 remaining = mgr.RemainingSeconds();
        ch.PSendSysMessage(
            "|cffff8040恐怖化区域|r（{}个活跃，剩余{}）：",
            static_cast<uint32>(rot.slots.size()), FormatRemaining(remaining));

        std::vector<ActiveEvent> events;
        bool haveEvents = mgr.IsEventsEnabled();
        if (haveEvents)
            events = mgr.GetEventsSnapshot();
        uint64 now = static_cast<uint64>(::time(nullptr));

        for (ActiveSlot const& s : rot.slots)
        {
            ch.PSendSysMessage("  |cffffd100{}|r — {} {}",
                               s.displayName,
                               TierDisplayName(s.tier),
                               FlavorDisplayName(s.flavor));
            if (!haveEvents)
                continue;
            for (ActiveEvent const& e : events)
            {
                if (e.zoneId != s.zoneId)
                    continue;
                if (e.state == EVENT_STATE_EXPIRED)
                    continue;
                ch.PSendSysMessage("      * {}: {} — {}",
                    EventTypeDisplayName(e.type),
                    e.displayName.empty() ? "（未命名）" : e.displayName,
                    FormatEventRemaining(now, e.startsAt, e.endsAt));
            }
        }
    }

    void PrintNext(Player* player)
    {
        auto& mgr = TerrorZonesMgr::Instance();
        ChatHandler ch(player->GetSession());
        if (!mgr.IsEnabled())
        {
            ch.SendSysMessage("恐怖地带功能已禁用。");
            return;
        }
        uint64 next = mgr.GetNextTickAt();
        uint64 now = static_cast<uint64>(::time(nullptr));
        if (next <= now)
        {
            ch.SendSysMessage("The next rotation is imminent.");
            return;
        }
        uint32 delta = static_cast<uint32>(next - now);
        time_t nextT = static_cast<time_t>(next);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &nextT);
#else
        localtime_r(&nextT, &local);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%H:%M", &local);
        ch.PSendSysMessage("Next rotation at {} server time ({}).",
                           buf, FormatRemaining(delta));
    }

    void PrintHistory(Player* player)
    {
        auto& mgr = TerrorZonesMgr::Instance();
        ChatHandler ch(player->GetSession());
        if (!mgr.IsEnabled())
        {
            ch.SendSysMessage("恐怖地带功能已禁用。");
            return;
        }
        std::vector<HistoryTick> hist = mgr.GetHistory(5);
        if (hist.empty())
        {
            ch.SendSysMessage("No rotation history yet.");
            return;
        }
        uint64 now = static_cast<uint64>(::time(nullptr));
        ch.PSendSysMessage("Last {} rotation(s):",
                           static_cast<uint32>(hist.size()));
        for (HistoryTick const& h : hist)
        {
            std::string names;
            for (size_t i = 0; i < h.slots.size(); ++i)
            {
                if (i) names += ", ";
                names += h.slots[i].displayName;
                names += " (";
                names += TierDisplayName(h.slots[i].tier);
                names += ' ';
                names += FlavorDisplayName(h.slots[i].flavor);
                names += ")";
            }
            ch.PSendSysMessage("  {} — {}", names, RelativeAge(h.tickAt, now));
        }
    }

    void PrintHowItWorks(Player* player)
    {
        ChatHandler ch(player->GetSession());
        ch.SendSysMessage("|cffff8040恐怖地带说明：|r");
        ch.SendSysMessage("  - 每隔一段时间，每个大陆的一个区域会变成\"恐怖化\"，"
                          "其中的怪物等比提升到你的等级。");
        ch.SendSysMessage("  - 恐怖化区域提供额外经验、金币和更好的战利品。每次轮换有阶位（1-5，越高越强越富）"
                          "和偏向奖励的属性类型。");
        ch.SendSysMessage("  - 每个恐怖化区域在整个轮换期间有一个世界Boss游荡——在小地图上追踪并击杀它"
                          "获取特殊掉落。");
        ch.SendSysMessage("  - 询问任何旅店老板，或输入 |cff00ff00.zones|r，查看当前恐怖化的区域。");
    }

    class TerrorZones_GossipCreature : public AllCreatureScript
    {
    public:
        TerrorZones_GossipCreature()
            : AllCreatureScript("TerrorZones_GossipCreature") {}

        bool OnCreatureGossipHelloAppend(Player* player,
                                         Creature* creature) override
        {
            if (!TerrorZonesMgr::Instance().IsInnkeeperGossipEnabled())
                return false;
            if (!creature->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER))
                return false;

            // Additive hook — core already prepared + will send the native
            // innkeeper menu. We only append our entry.
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE,
                "恐怖地带 - 当前强化区域？",
                SENDER_TZ, ACTION_TZ_OPEN);
            return true;
        }

        bool CanCreatureGossipSelect(Player* player, Creature* creature,
                                     uint32 sender, uint32 action) override
        {
            if (sender != SENDER_TZ)
                return false;   // not ours — let native handling proceed

            switch (action)
            {
                case ACTION_TZ_OPEN:
                case ACTION_TZ_BACK:
                    SendTzMenu(player, creature);
                    break;
                case ACTION_TZ_INFO:
                    PrintInfo(player);
                    SendTzMenu(player, creature);
                    break;
                case ACTION_TZ_NEXT:
                    PrintNext(player);
                    SendTzMenu(player, creature);
                    break;
                case ACTION_TZ_HISTORY:
                    PrintHistory(player);
                    SendTzMenu(player, creature);
                    break;
                case ACTION_TZ_HOW:
                    PrintHowItWorks(player);
                    SendTzMenu(player, creature);
                    break;
                case ACTION_TZ_CLOSE:
                default:
                    CloseGossipMenuFor(player);
                    break;
            }
            return true;   // we consumed the selection
        }
    };
}

void AddTerrorZonesGossipScripts()
{
    new TerrorZones_GossipCreature();
}
