#include "TerrorZonesMgr.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using namespace Acore::ChatCommands;
using namespace mod_terror_zones;

namespace
{
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
                return "将在" + std::to_string(delta) + "秒后触发";
            return "将在" + std::to_string(delta / 60) + "分后触发";
        }
        if (now >= endsAt)
            return "已过期";
        uint64 delta = endsAt - now;
        if (delta < 60)
            return "剩余" + std::to_string(delta) + "秒";
        return "剩余" + std::to_string(delta / 60) + "分";
    }

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

    std::string FormatAxisLine(RewardAxis axis, float value,
                                char const* tag)
    {
        char buf[96];
        if (IsProbabilityAxis(axis))
        {
            if (tag && *tag)
                std::snprintf(buf, sizeof(buf),
                              "    %-11s %5.1f%%  (%s)",
                              AxisShortName(axis),
                              value * 100.0f, tag);
            else
                std::snprintf(buf, sizeof(buf),
                              "    %-11s %5.1f%%",
                              AxisShortName(axis),
                              value * 100.0f);
        }
        else
        {
            if (tag && *tag)
                std::snprintf(buf, sizeof(buf),
                              "    %-11s x%.2f   (%s)",
                              AxisShortName(axis), value, tag);
            else
                std::snprintf(buf, sizeof(buf),
                              "    %-11s x%.2f",
                              AxisShortName(axis), value);
        }
        return std::string(buf);
    }

    bool HandleZonesInfo(ChatHandler* handler, char const* /*args*/)
    {
        auto& mgr = TerrorZonesMgr::Instance();
        if (!mgr.IsEnabled())
        {
            handler->SendSysMessage("恐怖地带功能已禁用。");
            return true;
        }

        ActiveRotation rot = mgr.GetActiveRotation();
        if (rot.slots.empty())
        {
            handler->SendSysMessage("尚无强化区域。请等待下一轮轮换。");
            return true;
        }

        uint32 remaining = mgr.RemainingSeconds();
        handler->PSendSysMessage(
            "|cffff8040强化区域|r（{}个槽位，剩余{}）：",
            static_cast<uint32>(rot.slots.size()), FormatRemaining(remaining));
        for (ActiveSlot const& s : rot.slots)
        {
            handler->PSendSysMessage("  |cffffd100{}|r — {} {}（区域 {}）",
                                     s.displayName,
                                     TierDisplayName(s.tier),
                                     FlavorDisplayName(s.flavor),
                                     s.zoneId);
            if (mgr.IsTierEnabled() && s.tier != TIER_NONE
                && s.flavor != FLAVOR_NONE)
            {
                FlavorBiasDef const& bias = FlavorBiasOf(s.flavor);
                for (uint32 a = 0; a < AXIS_COUNT; ++a)
                {
                    RewardAxis axis = static_cast<RewardAxis>(a);
                    char const* tag = "";
                    if (bias.primary == axis)
                        tag = "signature";
                    else if (bias.secondaryA == axis
                             || bias.secondaryB == axis)
                        tag = "secondary";
                    float v = mgr.RollAxis(s, axis);
                    handler->PSendSysMessage("{}",
                        FormatAxisLine(axis, v, tag));
                }
            }

            // Slice 8 — Difficulty line. Shows the composed combat HP
            // + damage mults for the slot. Tier bonus displayed
            // separately so GMs can read both the baseline and the
            // tier contribution at a glance.
            if (mgr.IsCombatEnabled())
            {
                float hpTier  = mgr.GetTierHpBonus(s.tier);
                float dmgTier = mgr.GetTierDamageBonus(s.tier);
                float hpMult  = mgr.GetCombatHpMult() * hpTier;
                float dmgMult = mgr.GetCombatDamageMult() * dmgTier;
                uint32 tierNum = (s.tier == TIER_NONE)
                    ? 1u : static_cast<uint32>(s.tier);
                handler->PSendSysMessage(
                    "    Difficulty  HP x{:.2f} (T{} +{:.2f}x), "
                    "Damage x{:.2f}",
                    hpMult, tierNum, hpTier, dmgMult);

                // Slice 8b — show elite-density only when the slot's
                // tier actually carries promotion (T1/T2 default 0).
                uint32 densityPm = mgr.GetEliteDensityPerMille(s.tier);
                if (densityPm > 0)
                {
                    float eliteHp  = mgr.GetEliteHpUplift();
                    float eliteDmg = mgr.GetEliteDamageUplift();
                    handler->PSendSysMessage(
                        "    Elite       {:.1f}% promoted "
                        "(HP x{:.2f}, Damage x{:.2f})",
                        densityPm / 10.0f, hpMult * eliteHp,
                        dmgMult * eliteDmg);
                }
            }
        }

        // Slice 6 — active/pending events per slot.
        if (mgr.IsEventsEnabled())
        {
            std::vector<ActiveEvent> events = mgr.GetEventsSnapshot();
            uint64 now = static_cast<uint64>(::time(nullptr));
            for (ActiveSlot const& s : rot.slots)
            {
                bool header = false;
                for (ActiveEvent const& e : events)
                {
                    if (e.zoneId != s.zoneId)
                        continue;
                    if (e.state == EVENT_STATE_EXPIRED)
                        continue;
                    if (!header)
                    {
                        handler->PSendSysMessage(
                            "    活跃事件（{}）：", s.displayName);
                        header = true;
                    }
                    // Slice 8 — event-boss HP mult shown inline so GMs
                    // can eyeball the full combat profile without
                    // inspecting the creature.
                    if (e.type == EVENT_WORLD_BOSS && mgr.IsCombatEnabled())
                    {
                        float hpTier = mgr.GetTierHpBonus(s.tier);
                        float hpMult = mgr.GetCombatHpMult() * hpTier
                                     * mgr.GetEventBossHpUplift();
                        handler->PSendSysMessage(
                            "      * {}: {} — {} (HP x{:.2f})",
                            EventTypeDisplayName(e.type),
                            e.displayName.empty() ? "（未命名）" : e.displayName,
                            FormatEventRemaining(now, e.startsAt, e.endsAt),
                            hpMult);
                    }
                    else
                    {
                        handler->PSendSysMessage(
                            "      * {}: {} — {}",
                            EventTypeDisplayName(e.type),
                            e.displayName.empty() ? "（未命名）" : e.displayName,
                            FormatEventRemaining(now, e.startsAt, e.endsAt));
                    }
                }
            }
        }
        return true;
    }

    bool HandleZonesNext(ChatHandler* handler, char const* /*args*/)
    {
        auto& mgr = TerrorZonesMgr::Instance();
        if (!mgr.IsEnabled())
        {
            handler->SendSysMessage("恐怖地带功能已禁用。");
            return true;
        }
        uint64 next = mgr.GetNextTickAt();
        uint64 now = static_cast<uint64>(::time(nullptr));
        if (next <= now)
        {
            handler->SendSysMessage("下次轮换即将开始。");
            return true;
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
        handler->PSendSysMessage("下次轮换在服务器时间 {}（{}后）。",
                                 buf, FormatRemaining(delta));
        return true;
    }

    bool HandleZonesHistory(ChatHandler* handler, char const* /*args*/)
    {
        auto& mgr = TerrorZonesMgr::Instance();
        if (!mgr.IsEnabled())
        {
            handler->SendSysMessage("恐怖地带功能已禁用。");
            return true;
        }
        std::vector<HistoryTick> hist = mgr.GetHistory(6);
        if (hist.empty())
        {
            handler->SendSysMessage("暂无轮换记录。");
            return true;
        }

        uint64 now = static_cast<uint64>(::time(nullptr));
        handler->PSendSysMessage("最近 {} 次轮换：",
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
            handler->PSendSysMessage("  {} — {}", names,
                                     RelativeAge(h.tickAt, now));
        }
        return true;
    }

    namespace
    {
        std::vector<std::string> SplitArgs(std::string const& s)
        {
            std::vector<std::string> out;
            std::string tok;
            for (char c : s)
            {
                if (c == ' ' || c == '\t')
                {
                    if (!tok.empty()) { out.push_back(tok); tok.clear(); }
                }
                else
                    tok.push_back(static_cast<char>(std::tolower(c)));
            }
            if (!tok.empty()) out.push_back(tok);
            return out;
        }

        bool ParseOnOff(std::string const& arg, bool& out)
        {
            if (arg == "on" || arg == "1" || arg == "true")
            { out = true; return true; }
            if (arg == "off" || arg == "0" || arg == "false")
            { out = false; return true; }
            return false;
        }

        void PrintAnnounceStatus(ChatHandler* handler, Player* player)
        {
            auto& mgr = TerrorZonesMgr::Instance();
            bool master = mgr.IsAnnounceEnabled(player);
            uint8 player_mask = mgr.GetAnnounceCategories(player);
            uint8 global_mask = mgr.GetGlobalAnnounceCategoryMask();
            handler->PSendSysMessage(
                "恐怖地带公告：总开关 = {}。",
                master ? "ON" : "OFF");
            for (uint8 i = 0; i < ANNOUNCE_CATEGORY_COUNT; ++i)
            {
                AnnounceCategory cat = static_cast<AnnounceCategory>(i);
                uint8 bit = AnnounceCategoryBit(cat);
                bool g = (global_mask & bit) != 0;
                bool p = (player_mask & bit) != 0;
                handler->PSendSysMessage(
                    "  {}（{}）：服务器={} 你={}",
                    AnnounceCategoryDisplayName(cat),
                    AnnounceCategoryCommandKey(cat),
                    g ? "on" : "off",
                    p ? "on" : "off");
            }
            handler->SendSysMessage(
                "使用 .zones announce on|off — 总开关。");
            handler->SendSysMessage(
                "使用 .zones announce <类别> on|off — 按类别。别名：all(全部), event(事件), rotation-all(轮换), zone(区域)。");
            handler->SendSysMessage(
                "使用 .zones announce reset — 恢复默认。");
        }
    }

    bool HandleZonesAnnounce(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        auto& mgr = TerrorZonesMgr::Instance();
        std::vector<std::string> tokens = SplitArgs(args ? args : "");

        if (tokens.empty())
        {
            PrintAnnounceStatus(handler, player);
            return true;
        }

        // .zones announce reset
        if (tokens.size() == 1 && tokens[0] == "reset")
        {
            mgr.SetAnnounceEnabled(player, true);
            mgr.SetAnnounceCategories(player, ANNOUNCE_CATEGORY_ALL);
            handler->SendSysMessage(
                "Terror Zones announcements reset to defaults "
                "(master ON, all categories ON).");
            return true;
        }

        // .zones announce on|off — master toggle (legacy form).
        if (tokens.size() == 1)
        {
            bool on;
            if (ParseOnOff(tokens[0], on))
            {
                mgr.SetAnnounceEnabled(player, on);
                handler->PSendSysMessage(
                    "恐怖地带公告已{}（总开关）。",
                    on ? "ON" : "OFF");
                return true;
            }
            // Single token that isn't on/off/reset → treat as a
            // category status query (could fall through but more
            // useful to print the same per-category dump).
            handler->SendSysMessage(
                "Usage: .zones announce | on | off | reset | "
                "<cat> on|off");
            handler->SetSentErrorMessage(true);
            return false;
        }

        // .zones announce <cat> on|off
        if (tokens.size() == 2)
        {
            bool on;
            if (!ParseOnOff(tokens[1], on))
            {
                handler->SendSysMessage(
                    "第二个参数必须为 'on' 或 'off'。");
                handler->SetSentErrorMessage(true);
                return false;
            }
            uint8 bits = ParseAnnounceCategoryAlias(tokens[0].c_str());
            if (bits == 0)
            {
                AnnounceCategory cat =
                    ParseAnnounceCategoryKey(tokens[0].c_str());
                if (cat == ANNOUNCE_CATEGORY_COUNT)
                {
                    handler->PSendSysMessage(
                        "未知类别 '{}'。请使用以下之一：",
                        tokens[0]);
                    for (uint8 i = 0; i < ANNOUNCE_CATEGORY_COUNT; ++i)
                        handler->PSendSysMessage("  {}",
                            AnnounceCategoryCommandKey(
                                static_cast<AnnounceCategory>(i)));
                    handler->SetSentErrorMessage(true);
                    return false;
                }
                bits = AnnounceCategoryBit(cat);
            }
            uint8 mask = mgr.GetAnnounceCategories(player);
            if (on)
                mask |= bits;
            else
                mask &= static_cast<uint8>(~bits);
            mgr.SetAnnounceCategories(player, mask);
            handler->PSendSysMessage(
                "恐怖地带公告：{} 类别现在已{}。",
                tokens[0], on ? "ON" : "OFF");
            return true;
        }

        handler->SendSysMessage(
            "Usage: .zones announce | on | off | reset | "
            "<cat> on|off");
        handler->SetSentErrorMessage(true);
        return false;
    }

    bool HandleZonesTick(ChatHandler* handler, char const* /*args*/)
    {
        auto& mgr = TerrorZonesMgr::Instance();
        if (!mgr.IsEnabled())
        {
            handler->SendSysMessage("恐怖地带功能已禁用。");
            return true;
        }
        mgr.ForceTick();
        handler->SendSysMessage("已强制触发一次恐怖地带轮换。");
        return true;
    }

    Flavor ParseFlavorKey(std::string const& key)
    {
        std::string s = key;
        for (char& c : s) c = static_cast<char>(std::tolower(c));
        if (s == "bloodbath")            return FLAVOR_BLOODBATH;
        if (s == "prospectors" || s == "prospector's" || s == "prospector")
            return FLAVOR_PROSPECTORS;
        if (s == "warlords" || s == "warlord's" || s == "warlord")
            return FLAVOR_WARLORDS;
        if (s == "arcane")               return FLAVOR_ARCANE;
        if (s == "merchants" || s == "merchant's" || s == "merchant")
            return FLAVOR_MERCHANTS;
        return FLAVOR_NONE;
    }

    bool HandleZonesTestWeather(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;
        if (!args || !*args)
        {
            handler->SendSysMessage(
                "用法: .zones testweather <状态> <强度>  "
                "(状态: 0=晴朗 1=雾 3-5=雨 6-8=雪 22/41/42=沙暴 "
                "86=雷暴 90=黑雨; 强度 0.0-1.0)");
            return true;
        }
        int state = 0;
        float grade = 0.0f;
        if (std::sscanf(args, "%d %f", &state, &grade) != 2)
        {
            handler->SendSysMessage("参数错误。示例: .zones testweather 90 0.75");
            handler->SetSentErrorMessage(true);
            return false;
        }
        TerrorZonesMgr::Instance().TestApplyWeather(
            player, static_cast<uint32>(state), grade);
        handler->PSendSysMessage(
            "已将天气状态={} 强度={:.2f} 应用到当前区域。",
            state, grade);
        return true;
    }

    bool HandleZonesTestFlavor(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;
        std::string arg = args ? args : "";
        while (!arg.empty() && arg.front() == ' ')
            arg.erase(arg.begin());
        while (!arg.empty() && arg.back() == ' ')
            arg.pop_back();
        if (arg.empty())
        {
            handler->SendSysMessage(
                "用法: .zones testflavor bloodbath|prospectors|warlords|arcane|merchants");
            return true;
        }
        Flavor flavor = ParseFlavorKey(arg);
        if (flavor == FLAVOR_NONE)
        {
            handler->PSendSysMessage("未知属性类型 '{}'。", arg);
            handler->SetSentErrorMessage(true);
            return false;
        }
        TerrorZonesMgr::Instance().TestApplyFlavor(player, flavor);
        handler->PSendSysMessage(
            "已将配置的 {} 氛围应用到当前区域。",
            FlavorDisplayName(flavor));
        return true;
    }

    bool HandleZonesSetFlavor(ChatHandler* handler, char const* args)
    {
        std::string arg = args ? args : "";
        while (!arg.empty() && arg.front() == ' ')
            arg.erase(arg.begin());
        while (!arg.empty() && arg.back() == ' ')
            arg.pop_back();
        if (arg.empty())
        {
            handler->SendSysMessage(
                "用法: .zones setflavor bloodbath|prospectors|warlords|arcane|merchants");
            return true;
        }
        Flavor flavor = ParseFlavorKey(arg);
        if (flavor == FLAVOR_NONE)
        {
            handler->PSendSysMessage("未知属性类型 '{}'。", arg);
            handler->SetSentErrorMessage(true);
            return false;
        }
        if (!TerrorZonesMgr::Instance().SetActiveFlavor(flavor))
        {
            handler->SendSysMessage(
                "没有活跃的轮换可重新标记。请先运行 .zones tick。");
            handler->SetSentErrorMessage(true);
            return false;
        }
        handler->PSendSysMessage(
            "Set active rotation flavor to {}. Rewards, gathering, and "
            "uniques now use this flavor's overlays.",
            FlavorDisplayName(flavor));
        return true;
    }

    bool HandleZonesTestClear(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;
        TerrorZonesMgr::Instance().TestClearAtmosphere(player);
        handler->SendSysMessage("已清除当前区域的天气覆盖。");
        return true;
    }

    bool HandleZonesSetTier(ChatHandler* handler, char const* args)
    {
        std::string arg = args ? args : "";
        while (!arg.empty() && arg.front() == ' ')
            arg.erase(arg.begin());
        while (!arg.empty() && arg.back() == ' ')
            arg.pop_back();
        if (arg.empty())
        {
            handler->SendSysMessage("用法: .zones settier <1-5>");
            return true;
        }
        long n = std::strtol(arg.c_str(), nullptr, 10);
        if (n < 1 || n > static_cast<long>(TIER_MAX))
        {
            handler->PSendSysMessage("阶位必须在 1-{} 之间。",
                                     static_cast<uint32>(TIER_MAX));
            handler->SetSentErrorMessage(true);
            return false;
        }
        Tier tier = static_cast<Tier>(n);
        if (!TerrorZonesMgr::Instance().SetActiveTier(tier))
        {
            handler->SendSysMessage(
                "没有活跃的轮换可重新设阶。请先运行 .zones tick。");
            handler->SetSentErrorMessage(true);
            return false;
        }
        handler->PSendSysMessage(
            "Set active rotation tier to {}. Rewards now use this tier's "
            "roll bracket on every axis.",
            TierDisplayName(tier));
        return true;
    }

    bool HandleZonesEventList(ChatHandler* handler, char const* /*args*/)
    {
        auto& mgr = TerrorZonesMgr::Instance();
        std::vector<ActiveEvent> evts = mgr.GetEventsSnapshot();
        if (evts.empty())
        {
            handler->SendSysMessage("没有活跃或计划中的事件。");
            return true;
        }
        uint64 now = static_cast<uint64>(::time(nullptr));
        handler->PSendSysMessage("活跃和计划中的事件（{}）：",
                                 static_cast<uint32>(evts.size()));
        for (ActiveEvent const& e : evts)
        {
            handler->PSendSysMessage(
                "  [{}] zone={} / {}: {} — {}",
                EventStateDisplayName(e.state), e.zoneId,
                EventTypeDisplayName(e.type),
                e.displayName.empty() ? "（未命名）" : e.displayName,
                FormatEventRemaining(now, e.startsAt, e.endsAt));
        }
        return true;
    }

    bool HandleZonesEventFire(ChatHandler* handler, char const* args)
    {
        Player* gm = handler->GetPlayer();
        if (!gm)
            return false;
        std::string arg = args ? args : "";
        while (!arg.empty() && arg.front() == ' ')
            arg.erase(arg.begin());
        while (!arg.empty() && arg.back() == ' ')
            arg.pop_back();
        if (arg.empty())
        {
            handler->SendSysMessage(
                "用法: .zones event fire worldboss|nodes");
            return true;
        }
        EventType type = ParseEventTypeKey(arg.c_str());
        if (type == EVENT_NONE)
        {
            handler->PSendSysMessage("未知事件类型 '{}'。", arg);
            handler->SetSentErrorMessage(true);
            return false;
        }
        if (type == EVENT_TREASURE_CARAVAN || type == EVENT_CHAMPION_GROUNDS)
        {
            handler->PSendSysMessage(
                "事件类型 '{}' 已推迟到 Slice 6b — 尚未实现。",
                arg);
            handler->SetSentErrorMessage(true);
            return false;
        }
        uint32 id = TerrorZonesMgr::Instance().FireEventNow(gm, type);
        if (id == 0)
        {
            handler->PSendSysMessage(
                "Failed to fire {} event in zone {} — no content curated "
                "for this zone. Check terror_zones_event_{} for a row with "
                "zone_id={}.",
                EventTypeDisplayName(type), gm->GetZoneId(),
                type == EVENT_WORLD_BOSS ? "bosses" : "node_surges",
                gm->GetZoneId());
            handler->SetSentErrorMessage(true);
            return false;
        }
        handler->PSendSysMessage(
            "已触发 {} 事件 ID {}（区域 {}）。",
            EventTypeDisplayName(type), id, gm->GetZoneId());
        return true;
    }

    bool HandleZonesEventEnd(ChatHandler* handler, char const* /*args*/)
    {
        Player* gm = handler->GetPlayer();
        if (!gm)
            return false;
        uint32 zoneId = gm->GetZoneId();
        uint32 n = TerrorZonesMgr::Instance().EndActiveEventsInZone(zoneId);
        if (n == 0)
            handler->PSendSysMessage(
                "区域 {} 没有活跃或待处理的事件。", zoneId);
        else
            handler->PSendSysMessage(
                "已结束 {} 个事件（区域 {}）。", n, zoneId);
        return true;
    }

    bool HandleZonesPool(ChatHandler* handler, char const* /*args*/)
    {
        std::vector<PoolEntry> pool = TerrorZonesMgr::Instance().GetPool();
        if (pool.empty())
        {
            handler->SendSysMessage("区域池为空。");
            return true;
        }
        handler->PSendSysMessage("区域池（{}个条目）：",
                                 static_cast<uint32>(pool.size()));
        for (PoolEntry const& e : pool)
            handler->PSendSysMessage("  [{}] {} lvl {}-{} {}",
                                     e.zoneId, e.displayName,
                                     e.levelMin, e.levelMax,
                                     e.enabled ? "" : "(已禁用)");
        return true;
    }

    class TerrorZones_CommandScript : public CommandScript
    {
    public:
        TerrorZones_CommandScript() : CommandScript("TerrorZones_CommandScript") {}

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable eventSub =
            {
                { "list", HandleZonesEventList, SEC_GAMEMASTER, Console::No },
                { "fire", HandleZonesEventFire, SEC_GAMEMASTER, Console::No },
                { "end",  HandleZonesEventEnd,  SEC_GAMEMASTER, Console::No },
            };
            static ChatCommandTable zonesSub =
            {
                { "",            HandleZonesInfo,        SEC_PLAYER,     Console::No },
                { "next",        HandleZonesNext,        SEC_PLAYER,     Console::No },
                { "history",     HandleZonesHistory,     SEC_PLAYER,     Console::No },
                { "announce",    HandleZonesAnnounce,    SEC_PLAYER,     Console::No },
                { "tick",        HandleZonesTick,        SEC_GAMEMASTER, Console::No },
                { "pool",        HandleZonesPool,        SEC_GAMEMASTER, Console::No },
                { "testweather", HandleZonesTestWeather, SEC_GAMEMASTER, Console::No },
                { "testflavor",  HandleZonesTestFlavor,  SEC_GAMEMASTER, Console::No },
                { "testclear",   HandleZonesTestClear,   SEC_GAMEMASTER, Console::No },
                { "setflavor",   HandleZonesSetFlavor,   SEC_GAMEMASTER, Console::No },
                { "settier",     HandleZonesSetTier,     SEC_GAMEMASTER, Console::No },
                { "event",       eventSub },
            };
            static ChatCommandTable root =
            {
                { "zones", zonesSub },
            };
            return root;
        }
    };
}

void AddTerrorZonesCommandScripts()
{
    new TerrorZones_CommandScript();
}
