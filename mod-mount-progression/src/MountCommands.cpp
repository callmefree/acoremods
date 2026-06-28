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
            handler->SendSysMessage("Mount progression is disabled.");
            return true;
        }

        uint32 activeSpell = mgr.GetActiveMount(player);
        if (!activeSpell)
        {
            handler->SendSysMessage("You have no active mount. Summon a mount to activate it.");
            return true;
        }

        CatalogEntry const* entry = mgr.GetCatalogEntry(activeSpell);
        if (!entry)
        {
            handler->PSendSysMessage("Active mount spell {} is not in the catalog.", activeSpell);
            return true;
        }

        MountProgress const* mp = mgr.GetProgress(player, activeSpell);
        uint16 level = mp ? mp->level : 1;
        uint32 xp = mp ? mp->xp : 0;
        uint32 need = mgr.XPToNextLevel(entry->rarity, level);

        handler->PSendSysMessage("Active mount: |cffffd100{}|r", entry->displayName);
        handler->PSendSysMessage("  Rarity: {}   Type: {}",
                                 RarityName(entry->rarity), TypeName(entry->type));
        if (need == 0)
            handler->PSendSysMessage("  Level: {} (max)", level);
        else
            handler->PSendSysMessage("  Level: {}   XP: {} / {}", level, xp, need);
        handler->PSendSysMessage("  XP source: {}", XPSourceName(entry->type));
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
            handler->SendSysMessage("Mount progression is disabled.");
            return true;
        }

        auto all = mgr.GetAllProgress(player);
        if (all.empty())
        {
            handler->SendSysMessage("You have no tracked mounts yet. Summon a mount to start.");
            return true;
        }

        std::sort(all.begin(), all.end(),
                  [](MountProgress const& a, MountProgress const& b) {
                      return a.level != b.level ? a.level > b.level : a.spellId < b.spellId;
                  });

        handler->PSendSysMessage("Tracked mounts ({}):", static_cast<uint32>(all.size()));
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
            handler->SendSysMessage("Usage: .mount give <spellId>");
            handler->SetSentErrorMessage(true);
            return false;
        }
        long spellId = std::strtol(args, nullptr, 10);
        if (spellId <= 0)
        {
            handler->SendSysMessage("spellId must be a positive integer.");
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
            handler->PSendSysMessage("Spell {} is not in the mount catalog.",
                                     static_cast<uint32>(spellId));
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!player->HasSpell(static_cast<uint32>(spellId)))
            player->learnSpell(static_cast<uint32>(spellId));

        mgr.ActivateMount(player, static_cast<uint32>(spellId));

        handler->PSendSysMessage(
            "Gave |cffffd100{}|r ({}, {}) and set as active mount.",
            entry->displayName, RarityName(entry->rarity), TypeName(entry->type));
        return true;
    }

    bool HandleMountAddXpCmd(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
        {
            handler->SendSysMessage("Usage: .mount addxp <amount>");
            handler->SetSentErrorMessage(true);
            return false;
        }
        long amount = std::strtol(args, nullptr, 10);
        if (amount <= 0)
        {
            handler->SendSysMessage("Amount must be a positive integer.");
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
        handler->PSendSysMessage("Awarded {} XP to your active mount.",
                                 static_cast<uint32>(amount));
        return true;
    }

    bool HandleMountSetLevelCmd(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
        {
            handler->SendSysMessage("Usage: .mount setlevel <level>");
            handler->SetSentErrorMessage(true);
            return false;
        }
        long level = std::strtol(args, nullptr, 10);
        if (level < 1)
        {
            handler->SendSysMessage("Level must be >= 1.");
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
        handler->PSendSysMessage("Set active mount to level {}.",
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
