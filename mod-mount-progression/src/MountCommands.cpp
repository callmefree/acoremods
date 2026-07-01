#include "MountProgressionMgr.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <algorithm>
#include <cstdlib>

using namespace Acore::ChatCommands;
using namespace mod_mount_progression;

namespace
{
    bool HandleMountInfoCmd(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        auto& mgr = MountProgressionMgr::Instance();
        if (!mgr.IsEnabled())
        {
            handler->SendSysMessage("坐骑进阶功能已禁用。");
            return true;
        }

        uint32 activeSpell = mgr.GetActiveMount(player);
        if (!activeSpell)
        {
            handler->SendSysMessage("你没有激活中的坐骑。召唤一只坐骑来激活它。");
            return true;
        }

        CatalogEntry const* entry = mgr.GetCatalogEntry(activeSpell);
        if (!entry)
        {
            handler->PSendSysMessage("激活的坐骑法术 {} 不在目录中。", activeSpell);
            return true;
        }

        MountProgress const* mp = mgr.GetProgress(player, activeSpell);
        uint16 level = mp ? mp->level : 1;
        uint32 xp = mp ? mp->xp : 0;
        uint32 need = mgr.XPToNextLevel(entry->rarity, level);

        handler->PSendSysMessage("当前坐骑：|cffffd100{}|r", entry->displayName);
        handler->PSendSysMessage("  稀有度：{}   类型：{}",
                                 RarityName(entry->rarity), TypeName(entry->type));
        if (need == 0)
            handler->PSendSysMessage("  等级：{}（满级）", level);
        else
            handler->PSendSysMessage("  等级：{}   经验：{} / {}", level, xp, need);
        handler->PSendSysMessage("  经验来源：{}", XPSourceName(entry->type));
        return true;
    }

    bool HandleMountsListCmd(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        auto& mgr = MountProgressionMgr::Instance();
        if (!mgr.IsEnabled())
        {
            handler->SendSysMessage("坐骑进阶功能已禁用。");
            return true;
        }

        auto all = mgr.GetAllProgress(player);
        if (all.empty())
        {
            handler->SendSysMessage("你还没有追踪的坐骑。召唤一只坐骑来开始。");
            return true;
        }

        std::sort(all.begin(), all.end(),
                  [](MountProgress const& a, MountProgress const& b) {
                      return a.level != b.level ? a.level > b.level : a.spellId < b.spellId;
                  });

        handler->PSendSysMessage("已追踪的坐骑（{}）：", static_cast<uint32>(all.size()));
        for (MountProgress const& mp : all)
        {
            CatalogEntry const* entry = mgr.GetCatalogEntry(mp.spellId);
            char const* name = entry ? entry->displayName.c_str() : "(unknown)";
            char const* rarity = entry ? RarityName(entry->rarity) : "?";
            handler->PSendSysMessage("  [{}] {} ({})   lvl {}  xp {}",
                                     mp.spellId, name, rarity, mp.level, mp.xp);
        }
        return true;
    }

    bool HandleMountGiveCmd(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
        {
            handler->SendSysMessage("用法: .mount give <法术ID>");
            handler->SetSentErrorMessage(true);
            return false;
        }
        long spellId = std::strtol(args, nullptr, 10);
        if (spellId <= 0)
        {
            handler->SendSysMessage("法术ID必须为正整数。");
            handler->SetSentErrorMessage(true);
            return false;
        }
        Player* player = handler->GetPlayer();
        if (!player)
            return false;

        auto& mgr = MountProgressionMgr::Instance();
        CatalogEntry const* entry = mgr.GetCatalogEntry(static_cast<uint32>(spellId));
        if (!entry)
        {
            handler->PSendSysMessage("法术 {} 不在坐骑目录中。",
                                     static_cast<uint32>(spellId));
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!player->HasSpell(static_cast<uint32>(spellId)))
            player->learnSpell(static_cast<uint32>(spellId));

        mgr.ActivateMount(player, static_cast<uint32>(spellId));

        handler->PSendSysMessage(
            "已给予坐骑 |cffffd100{}|r（{}，{}）并设为当前坐骑。",
            entry->displayName, RarityName(entry->rarity), TypeName(entry->type));
        return true;
    }

    bool HandleMountAddXpCmd(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
        {
            handler->SendSysMessage("用法: .mount addxp <数量>");
            handler->SetSentErrorMessage(true);
            return false;
        }
        long amount = std::strtol(args, nullptr, 10);
        if (amount <= 0)
        {
            handler->SendSysMessage("数量必须为正整数。");
            handler->SetSentErrorMessage(true);
            return false;
        }
        Player* player = handler->GetPlayer();
        if (!player)
            return false;
        if (!MountProgressionMgr::Instance().AwardActiveMountXP(
                player, static_cast<uint32>(amount)))
        {
            handler->SendSysMessage("No active mount (or not in catalog). "
                                    "Mount up first.");
            return true;
        }
        handler->PSendSysMessage("已为当前坐骑增加 {} 点经验。",
                                 static_cast<uint32>(amount));
        return true;
    }

    bool HandleMountSetLevelCmd(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
        {
            handler->SendSysMessage("用法: .mount setlevel <等级>");
            handler->SetSentErrorMessage(true);
            return false;
        }
        long level = std::strtol(args, nullptr, 10);
        if (level < 1)
        {
            handler->SendSysMessage("等级必须 >= 1。");
            handler->SetSentErrorMessage(true);
            return false;
        }
        Player* player = handler->GetPlayer();
        if (!player)
            return false;
        if (!MountProgressionMgr::Instance().SetActiveMountLevel(
                player, static_cast<uint16>(level)))
        {
            handler->SendSysMessage("No active mount (or not in catalog). "
                                    "Mount up first.");
            return true;
        }
        handler->PSendSysMessage("已将当前坐骑设为等级 {}。",
                                 static_cast<uint32>(level));
        return true;
    }

    class Mount_CommandScript : public CommandScript
    {
    public:
        Mount_CommandScript() : CommandScript("Mount_CommandScript") {}

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable mountSub =
            {
                { "",         HandleMountInfoCmd,     SEC_PLAYER,     Console::No },
                { "give",     HandleMountGiveCmd,     SEC_GAMEMASTER, Console::No },
                { "addxp",    HandleMountAddXpCmd,    SEC_GAMEMASTER, Console::No },
                { "setlevel", HandleMountSetLevelCmd, SEC_GAMEMASTER, Console::No },
            };
            static ChatCommandTable root =
            {
                { "mount",  mountSub },
                { "mounts", HandleMountsListCmd, SEC_PLAYER, Console::No },
            };
            return root;
        }
    };
}

void AddMountProgressionCommandScripts()
{
    new Mount_CommandScript();
}
