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
            return "just now";
        uint64 delta = now - tickAt;
        if (delta < 60)
            return std::to_string(delta) + "s ago";
        if (delta < 3600)
            return std::to_string(delta / 60) + "m ago";
        if (delta < 86400)
            return std::to_string(delta / 3600) + "h ago";
        return std::to_string(delta / 86400) + "d ago";
    }

    std::string FormatEventRemaining(uint64 now, uint64 startsAt, uint64 endsAt)
    {
        if (now < startsAt)
        {
            uint64 delta = startsAt - now;
            if (delta < 60)
                return "fires in " + std::to_string(delta) + "s";
            return "fires in " + std::to_string(delta / 60) + "m";
        }
        if (now >= endsAt)
            return "expired";
        uint64 delta = endsAt - now;
        if (delta < 60)
            return std::to_string(delta) + "s remaining";
        return std::to_string(delta / 60) + "m remaining";
    }

    std::string FormatRemaining(uint32 secs)
    {
        if (secs >= 3600)
        {
            uint32 h = secs / 3600;
            uint32 m = (secs % 3600) / 60;
            if (m == 0)
                return (h == 1) ? "1 hour" : std::to_string(h) + " hours";
            return std::to_string(h) + "h " + std::to_string(m) + "m";
        }
        if (secs >= 60)
        {
            uint32 m = secs / 60;
            return (m == 1) ? "1 minute" : std::to_string(m) + " minutes";
        }
        return std::to_string(secs) + "s";
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
            handler->SendSysMessage("Terror Zones is disabled.");
            return true;
        }

        ActiveRotation rot = mgr.GetActiveRotation();
        if (rot.slots.empty())
        {
            handler->SendSysMessage("No empowered zone yet. Wait for the next rotation.");
            return true;
        }

        uint32 remaining = mgr.RemainingSeconds();
        handler->PSendSysMessage(
            "|cffff8040Empowered zone(s)|r ({} slot(s), {} remaining):",
            static_cast<uint32>(rot.slots.size()), FormatRemaining(remaining));
        for (ActiveSlot const& s : rot.slots)
        {
            handler->PSendSysMessage("  |cffffd100{}|r — {} {} (zone {})",
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
                            "    Active events ({}):", s.displayName);
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
                            e.displayName.empty() ? "(unnamed)" : e.displayName,
                            FormatEventRemaining(now, e.startsAt, e.endsAt),
                            hpMult);
                    }
                    else
                    {
                        handler->PSendSysMessage(
                            "      * {}: {} — {}",
                            EventTypeDisplayName(e.type),
                            e.displayName.empty() ? "(unnamed)" : e.displayName,
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
            handler->SendSysMessage("Terror Zones is disabled.");
            return true;
        }
        uint64 next = mgr.GetNextTickAt();
        uint64 now = static_cast<uint64>(::time(nullptr));
        if (next <= now)
        {
            handler->SendSysMessage("Next rotation is imminent.");
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
        handler->PSendSysMessage("Next rotation at {} server time ({}).",
                                 buf, FormatRemaining(delta));
        return true;
    }

    bool HandleZonesHistory(ChatHandler* handler, char const* /*args*/)
    {
        auto& mgr = TerrorZonesMgr::Instance();
        if (!mgr.IsEnabled())
        {
            handler->SendSysMessage("Terror Zones is disabled.");
            return true;
        }
        std::vector<HistoryTick> hist = mgr.GetHistory(6);
        if (hist.empty())
        {
            handler->SendSysMessage("No rotation history yet.");
            return true;
        }

        uint64 now = static_cast<uint64>(::time(nullptr));
        handler->PSendSysMessage("Last {} rotation(s):",
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
                "Terror Zones announcements: master = {}.",
                master ? "ON" : "OFF");
            for (uint8 i = 0; i < ANNOUNCE_CATEGORY_COUNT; ++i)
            {
                AnnounceCategory cat = static_cast<AnnounceCategory>(i);
                uint8 bit = AnnounceCategoryBit(cat);
                bool g = (global_mask & bit) != 0;
                bool p = (player_mask & bit) != 0;
                handler->PSendSysMessage(
                    "  {} ({}): server={} you={}",
                    AnnounceCategoryDisplayName(cat),
                    AnnounceCategoryCommandKey(cat),
                    g ? "on" : "off",
                    p ? "on" : "off");
            }
            handler->SendSysMessage(
                "Use .zones announce on|off — master toggle.");
            handler->SendSysMessage(
                "Use .zones announce <cat> on|off — per-category. "
                "Aliases: all, event, rotation-all, zone.");
            handler->SendSysMessage(
                "Use .zones announce reset — restore defaults.");
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
                    "Terror Zones announcements {} (master).",
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
                    "Second arg must be 'on' or 'off'.");
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
                        "Unknown category '{}'. Use one of: ",
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
                "Terror Zones announcements: {} categories now {}.",
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
            handler->SendSysMessage("Terror Zones is disabled.");
            return true;
        }
        mgr.ForceTick();
        handler->SendSysMessage("Forced a Terror Zones rotation tick.");
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
                "Usage: .zones testweather <state> <grade>  "
                "(state: 0=FINE, 1=FOG, 3-5=rain, 6-8=snow, 22/41/42=sandstorm, "
                "86=THUNDERS, 90=BLACKRAIN; grade 0.0-1.0)");
            return true;
        }
        int state = 0;
        float grade = 0.0f;
        if (std::sscanf(args, "%d %f", &state, &grade) != 2)
        {
            handler->SendSysMessage("Bad args. Example: .zones testweather 90 0.75");
            handler->SetSentErrorMessage(true);
            return false;
        }
        TerrorZonesMgr::Instance().TestApplyWeather(
            player, static_cast<uint32>(state), grade);
        handler->PSendSysMessage(
            "Applied weather state={} grade={:.2f} to your current zone.",
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
                "Usage: .zones testflavor bloodbath|prospectors|warlords|arcane|merchants");
            return true;
        }
        Flavor flavor = ParseFlavorKey(arg);
        if (flavor == FLAVOR_NONE)
        {
            handler->PSendSysMessage("Unknown flavor '{}'.", arg);
            handler->SetSentErrorMessage(true);
            return false;
        }
        TerrorZonesMgr::Instance().TestApplyFlavor(player, flavor);
        handler->PSendSysMessage(
            "Applied configured {} atmosphere to your current zone.",
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
                "Usage: .zones setflavor bloodbath|prospectors|warlords|arcane|merchants");
            return true;
        }
        Flavor flavor = ParseFlavorKey(arg);
        if (flavor == FLAVOR_NONE)
        {
            handler->PSendSysMessage("Unknown flavor '{}'.", arg);
            handler->SetSentErrorMessage(true);
            return false;
        }
        if (!TerrorZonesMgr::Instance().SetActiveFlavor(flavor))
        {
            handler->SendSysMessage(
                "No active rotation to retag. Run .zones tick first.");
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
        handler->SendSysMessage("Cleared weather override on your current zone.");
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
            handler->SendSysMessage("Usage: .zones settier <1-5>");
            return true;
        }
        long n = std::strtol(arg.c_str(), nullptr, 10);
        if (n < 1 || n > static_cast<long>(TIER_MAX))
        {
            handler->PSendSysMessage("Tier must be 1-{}.",
                                     static_cast<uint32>(TIER_MAX));
            handler->SetSentErrorMessage(true);
            return false;
        }
        Tier tier = static_cast<Tier>(n);
        if (!TerrorZonesMgr::Instance().SetActiveTier(tier))
        {
            handler->SendSysMessage(
                "No active rotation to retier. Run .zones tick first.");
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
            handler->SendSysMessage("No active or scheduled events.");
            return true;
        }
        uint64 now = static_cast<uint64>(::time(nullptr));
        handler->PSendSysMessage("Active + scheduled events ({}):",
                                 static_cast<uint32>(evts.size()));
        for (ActiveEvent const& e : evts)
        {
            handler->PSendSysMessage(
                "  [{}] zone={} / {}: {} — {}",
                EventStateDisplayName(e.state), e.zoneId,
                EventTypeDisplayName(e.type),
                e.displayName.empty() ? "(unnamed)" : e.displayName,
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
                "Usage: .zones event fire worldboss|nodes");
            return true;
        }
        EventType type = ParseEventTypeKey(arg.c_str());
        if (type == EVENT_NONE)
        {
            handler->PSendSysMessage("Unknown event type '{}'.", arg);
            handler->SetSentErrorMessage(true);
            return false;
        }
        if (type == EVENT_TREASURE_CARAVAN || type == EVENT_CHAMPION_GROUNDS)
        {
            handler->PSendSysMessage(
                "Event type '{}' is deferred to Slice 6b — not implemented.",
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
            "Fired {} event (id {}) in zone {}.",
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
                "No active or pending events in zone {}.", zoneId);
        else
            handler->PSendSysMessage(
                "Ended {} event(s) in zone {}.", n, zoneId);
        return true;
    }

    bool HandleZonesPool(ChatHandler* handler, char const* /*args*/)
    {
        std::vector<PoolEntry> pool = TerrorZonesMgr::Instance().GetPool();
        if (pool.empty())
        {
            handler->SendSysMessage("Zone pool is empty.");
            return true;
        }
        handler->PSendSysMessage("Zone pool ({} entries):",
                                 static_cast<uint32>(pool.size()));
        for (PoolEntry const& e : pool)
            handler->PSendSysMessage("  [{}] {} lvl {}-{} {}",
                                     e.zoneId, e.displayName,
                                     e.levelMin, e.levelMax,
                                     e.enabled ? "" : "(disabled)");
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
