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
#include "Config.h"
#include "Creature.h"
#include "GossipDef.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"

namespace
{
    // A distinctive sender id so our gossip selections never collide with the
    // innkeeper's database options (which always carry sender 0).
    enum BagSorterGossip : uint32
    {
        SENDER_BAGSORT          = 0xB465,

        ACTION_OPEN             = 1000,
        ACTION_SORT_TYPE        = 1001,
        ACTION_SORT_QUALITY     = 1002,
        ACTION_SORT_ILVL        = 1003,
        ACTION_SORT_NAME        = 1004,
        ACTION_SORT_QUEST       = 1005,
        ACTION_CANCEL           = 1006,
    };

    void RunSortAndReport(Player* player, BagSorter::SortMode mode)
    {
        uint32 const count = BagSorter::Sort(player, mode);

        if (BagSorter::settings.Announce)
        {
            ChatHandler handler(player->GetSession());
            if (count > 0)
                handler.PSendSysMessage("Your bags have been organized ({} items).", count);
            else
                handler.SendSysMessage("There was nothing to organize.");
        }

        CloseGossipMenuFor(player);
    }
}

class mod_bag_sorter_creature : public AllCreatureScript
{
public:
    mod_bag_sorter_creature() : AllCreatureScript("mod_bag_sorter_creature") { }

    bool OnCreatureGossipHelloAppend(Player* player, Creature* creature) override
    {
        if (!BagSorter::settings.Enable || !creature->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER))
            return false;

        // Additive hook: the core has already prepared the innkeeper's native
        // menu (rest/bind, vendor, etc.) and sends it once every module has
        // appended. We only add our option - no Prepare/Send here, so we
        // coexist with other modules (e.g. mod-terror-zones) on the same NPC.
        AddGossipItemFor(player, GOSSIP_ICON_TABARD, "整理我的背包", SENDER_BAGSORT, ACTION_OPEN);
        return true; // we appended an item
    }

    bool CanCreatureGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        if (sender != SENDER_BAGSORT)
            return false; // not ours - let native handling (set home, etc.) proceed

        switch (action)
        {
            case ACTION_OPEN:
                ClearGossipMenuFor(player);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "按类型和品质", SENDER_BAGSORT, ACTION_SORT_TYPE);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "按品质", SENDER_BAGSORT, ACTION_SORT_QUALITY);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "按物品等级", SENDER_BAGSORT, ACTION_SORT_ILVL);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "按名称（A-Z）", SENDER_BAGSORT, ACTION_SORT_NAME);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "按类型品质，任务物品放最后",
                    SENDER_BAGSORT, ACTION_SORT_QUEST);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Nevermind", SENDER_BAGSORT, ACTION_CANCEL);
                SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
                break;
            case ACTION_SORT_TYPE:
                RunSortAndReport(player, BagSorter::SortMode::TypeQuality);
                break;
            case ACTION_SORT_QUALITY:
                RunSortAndReport(player, BagSorter::SortMode::Quality);
                break;
            case ACTION_SORT_ILVL:
                RunSortAndReport(player, BagSorter::SortMode::ItemLevel);
                break;
            case ACTION_SORT_NAME:
                RunSortAndReport(player, BagSorter::SortMode::Name);
                break;
            case ACTION_SORT_QUEST:
                RunSortAndReport(player, BagSorter::SortMode::TypeQualityQuestLast);
                break;
            case ACTION_CANCEL:
            default:
                CloseGossipMenuFor(player);
                break;
        }

        return true; // we consumed the selection
    }
};

class mod_bag_sorter_world : public WorldScript
{
public:
    mod_bag_sorter_world() : WorldScript("mod_bag_sorter_world") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        BagSorter::settings.Enable         = sConfigMgr->GetOption<bool>("BagSorter.Enable", true);
        BagSorter::settings.MergeStacks    = sConfigMgr->GetOption<bool>("BagSorter.MergeStacks", true);
        BagSorter::settings.Announce       = sConfigMgr->GetOption<bool>("BagSorter.Announce", true);
        BagSorter::settings.PinHearthstone = sConfigMgr->GetOption<bool>("BagSorter.PinHearthstone", true);
    }
};

void AddSC_mod_bag_sorter_gossip()
{
    new mod_bag_sorter_creature();
    new mod_bag_sorter_world();
}
