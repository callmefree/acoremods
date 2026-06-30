/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "mod_bag_sorter.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Player.h"
#include "RBAC.h"

using namespace Acore::ChatCommands;

class mod_bag_sorter_commandscript : public CommandScript
{
public:
    mod_bag_sorter_commandscript() : CommandScript("mod_bag_sorter_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            // Reuses a permission held by the default player role so any player
            // may run it (the diegetic path is the innkeeper gossip).
            { "sortbags", HandleSortBagsCommand, rbac::RBAC_PERM_COMMAND_DISMOUNT, Console::No },
        };

        return commandTable;
    }

    static bool HandleSortBagsCommand(ChatHandler* handler, Optional<std::string_view> modeArg)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        if (!BagSorter::settings.Enable)
        {
            handler->SendSysMessage("Bag sorting is disabled on this server.");
            return true;
        }

        BagSorter::SortMode mode = BagSorter::SortMode::TypeQuality;
        if (modeArg)
        {
            std::string_view arg = *modeArg;
            if (arg == "type" || arg == "typequality")
                mode = BagSorter::SortMode::TypeQuality;
            else if (arg == "quality")
                mode = BagSorter::SortMode::Quality;
            else if (arg == "ilvl" || arg == "itemlevel")
                mode = BagSorter::SortMode::ItemLevel;
            else if (arg == "name")
                mode = BagSorter::SortMode::Name;
            else if (arg == "questlast" || arg == "quest")
                mode = BagSorter::SortMode::TypeQualityQuestLast;
            else
            {
                handler->SendSysMessage("Usage: .sortbags [type|quality|ilvl|name|questlast]");
                return true;
            }
        }

        uint32 const count = BagSorter::Sort(player, mode);
        if (count > 0)
            handler->PSendSysMessage("Your bags have been organized ({} items).", count);
        else
            handler->SendSysMessage("There was nothing to organize.");

        return true;
    }
};

void AddSC_mod_bag_sorter_command()
{
    new mod_bag_sorter_commandscript();
}
