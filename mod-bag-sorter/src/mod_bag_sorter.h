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

#ifndef MOD_BAG_SORTER_H
#define MOD_BAG_SORTER_H

#include "Define.h"

class Player;

namespace BagSorter
{
    enum class SortMode : uint8
    {
        TypeQuality          = 0,    // class/subclass/type, then quality desc, then ilvl desc, then name
        Quality              = 1,    // quality desc, then name
        ItemLevel            = 2,    // ilvl desc, then quality desc, then name
        Name                 = 3,    // name A-Z
        TypeQualityQuestLast = 4,    // TypeQuality ordering, then quest items swept into the last bag
    };

    struct Settings
    {
        bool Enable         = true;
        bool MergeStacks    = true;
        bool Announce       = true;
        bool PinHearthstone = true;  // always place the Hearthstone in the backpack's first slot
    };

    // Runtime config, populated from worldserver config in OnAfterConfigLoad.
    extern Settings settings;

    // Sorts the player's carried bags (backpack + the 4 equipped bag containers)
    // using the chosen mode. Returns the number of items that were arranged.
    // Specialized (profession) bags are sorted internally; general bags + the
    // backpack are sorted together as one pool. Never moves items across pools,
    // so every move is a guaranteed-valid SwapItem.
    //
    // If PinHearthstone is set, the Hearthstone is forced into the backpack's
    // first slot for every mode. In TypeQualityQuestLast, quest-class items are
    // additionally packed into the tail of the general pool (the last bag).
    uint32 Sort(Player* player, SortMode mode);
}

#endif // MOD_BAG_SORTER_H
