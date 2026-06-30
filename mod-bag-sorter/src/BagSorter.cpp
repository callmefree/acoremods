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
#include "Bag.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Player.h"
#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace BagSorter
{
    Settings settings;
}

using namespace BagSorter;

namespace
{
    constexpr uint32 HEARTHSTONE_ITEM_ENTRY = 6948;

    // Pack a (bag, slot) pair into the uint16 position used by Player::SwapItem.
    inline uint16 PackPos(uint8 bag, uint8 slot)
    {
        return (uint16(bag) << 8) | slot;
    }

    // A "pool" is an ordered list of slot positions that items may freely move
    // between. pools[0] is always the general pool (backpack first, then every
    // general BagFamily == 0 equipped bag); each specialized bag forms its own
    // pool so we never attempt a move a bag family would reject.
    void BuildPools(Player* player, std::vector<std::vector<uint16>>& pools)
    {
        std::vector<uint16> general;

        // Backpack (INVENTORY_SLOT_BAG_0, slots 23..38). slots[0] here is the
        // backpack's first slot - the "bag 0, slot 0" the Hearthstone is pinned to.
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            general.push_back(PackPos(INVENTORY_SLOT_BAG_0, slot));

        // The 4 equipped bag containers (bag slots 19..22), in order, so the last
        // equipped general bag lands at the tail of the general pool.
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        {
            Bag* pBag = player->GetBagByPos(bag);
            if (!pBag)
                continue;

            ItemTemplate const* bagTemplate = pBag->GetTemplate();
            bool const isSpecialized = bagTemplate && bagTemplate->BagFamily != 0;

            if (isSpecialized)
            {
                std::vector<uint16> special;
                for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                    special.push_back(PackPos(bag, uint8(j)));

                pools.push_back(std::move(special));
            }
            else
            {
                for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                    general.push_back(PackPos(bag, uint8(j)));
            }
        }

        pools.insert(pools.begin(), std::move(general));
    }

    // Merge partial stacks of the same item within a pool. Relies on
    // Player::SwapItem, which fills the destination stack from the source and
    // leaves any overflow in the source - exactly like a client drag-merge.
    void ConsolidatePool(Player* player, std::vector<uint16> const& slots)
    {
        std::unordered_map<uint32, std::vector<uint16>> byEntry;
        for (uint16 pos : slots)
            if (Item* item = player->GetItemByPos(pos))
                if (item->GetTemplate()->GetMaxStackSize() > 1)
                    byEntry[item->GetEntry()].push_back(pos);

        for (auto& kvp : byEntry)
        {
            std::vector<uint16>& positions = kvp.second;
            if (positions.size() < 2)
                continue;

            for (std::size_t d = 0; d < positions.size(); ++d)
            {
                Item* dst = player->GetItemByPos(positions[d]);
                if (!dst)
                    continue;

                uint32 const maxStack = dst->GetTemplate()->GetMaxStackSize();
                for (std::size_t s = d + 1; s < positions.size() && dst->GetCount() < maxStack; ++s)
                {
                    if (!player->GetItemByPos(positions[s]))
                        continue;

                    player->SwapItem(positions[s], positions[d]); // merges src -> dst
                    dst = player->GetItemByPos(positions[d]);
                    if (!dst)
                        break;
                }
            }
        }
    }

    bool CompareItems(Item* a, Item* b, SortMode mode)
    {
        // Quest-last shares the TypeQuality ordering; the quest sweep is applied
        // afterwards during placement, not in the comparator.
        if (mode == SortMode::TypeQualityQuestLast)
            mode = SortMode::TypeQuality;

        ItemTemplate const* ta = a->GetTemplate();
        ItemTemplate const* tb = b->GetTemplate();

        switch (mode)
        {
            case SortMode::TypeQuality:
                if (ta->Class != tb->Class)                 return ta->Class < tb->Class;
                if (ta->SubClass != tb->SubClass)           return ta->SubClass < tb->SubClass;
                if (ta->InventoryType != tb->InventoryType) return ta->InventoryType < tb->InventoryType;
                if (ta->Quality != tb->Quality)             return ta->Quality > tb->Quality;
                if (ta->ItemLevel != tb->ItemLevel)         return ta->ItemLevel > tb->ItemLevel;
                break;
            case SortMode::Quality:
                if (ta->Quality != tb->Quality)             return ta->Quality > tb->Quality;
                break;
            case SortMode::ItemLevel:
                if (ta->ItemLevel != tb->ItemLevel)         return ta->ItemLevel > tb->ItemLevel;
                if (ta->Quality != tb->Quality)             return ta->Quality > tb->Quality;
                break;
            case SortMode::Name:
            case SortMode::TypeQualityQuestLast:            // already remapped above
                break;
        }

        if (ta->Name1 != tb->Name1)
            return ta->Name1 < tb->Name1;

        return a->GetGUID() < b->GetGUID(); // stable, deterministic tiebreak
    }

    std::vector<Item*> CollectItems(Player* player, std::vector<uint16> const& slots)
    {
        std::vector<Item*> items;
        for (uint16 pos : slots)
            if (Item* item = player->GetItemByPos(pos))
                items.push_back(item);

        return items;
    }

    // Move each item to its assigned slot using validated swaps. 'targets' is a
    // partial slot->item assignment (slots with no target end empty). Identity is
    // tracked by ObjectGuid, re-resolved each step, so an item that is merged away
    // mid-pass is skipped rather than dereferenced stale. Target slots are
    // pairwise distinct, so this realizes the assignment in <= N swaps.
    uint32 ApplyTargets(Player* player, std::vector<uint16> const& slots,
                        std::vector<std::pair<uint16, ObjectGuid>> const& targets)
    {
        uint32 placed = 0;
        for (std::pair<uint16, ObjectGuid> const& target : targets)
        {
            uint16 const destPos = target.first;
            ObjectGuid const guid = target.second;

            uint16 curPos = 0xFFFF;
            for (uint16 pos : slots)
            {
                Item* item = player->GetItemByPos(pos);
                if (item && item->GetGUID() == guid)
                {
                    curPos = pos;
                    break;
                }
            }

            if (curPos == 0xFFFF)
                continue; // merged away during this pass

            if (curPos != destPos)
                player->SwapItem(curPos, destPos);

            ++placed;
        }

        return placed;
    }

    // Specialized bags: a plain sorted, front-packed placement.
    uint32 PlacePool(Player* player, std::vector<uint16> const& slots, SortMode mode)
    {
        std::vector<Item*> items = CollectItems(player, slots);
        if (items.empty())
            return 0;

        std::sort(items.begin(), items.end(), [mode](Item* a, Item* b)
        {
            return CompareItems(a, b, mode);
        });

        std::vector<std::pair<uint16, ObjectGuid>> targets;
        targets.reserve(items.size());
        for (std::size_t i = 0; i < items.size() && i < slots.size(); ++i)
            targets.emplace_back(slots[i], items[i]->GetGUID());

        return ApplyTargets(player, slots, targets);
    }

    // General pool: front-packed sorted items, with the Hearthstone optionally
    // pinned to the first slot and (in quest-last mode) quest items anchored to
    // the tail so they end up in the last bag, clear of everything else.
    uint32 PlaceGeneralPool(Player* player, std::vector<uint16> const& slots, SortMode mode)
    {
        std::vector<Item*> items = CollectItems(player, slots);
        if (items.empty())
            return 0;

        bool const questLast = (mode == SortMode::TypeQualityQuestLast);
        bool const pinHearth = settings.PinHearthstone;

        Item* hearthstone = nullptr;
        std::vector<Item*> quest;
        std::vector<Item*> rest;

        for (Item* item : items)
        {
            if (pinHearth && !hearthstone && item->GetEntry() == HEARTHSTONE_ITEM_ENTRY)
            {
                hearthstone = item;
                continue;
            }

            if (questLast && item->GetTemplate()->Class == ITEM_CLASS_QUEST)
                quest.push_back(item);
            else
                rest.push_back(item);
        }

        auto const cmp = [mode](Item* a, Item* b) { return CompareItems(a, b, mode); };
        std::sort(rest.begin(), rest.end(), cmp);
        std::sort(quest.begin(), quest.end(), cmp);

        std::vector<std::pair<uint16, ObjectGuid>> targets;
        targets.reserve(items.size());

        std::size_t idx = 0;
        if (hearthstone)
        {
            targets.emplace_back(slots[0], hearthstone->GetGUID());
            idx = 1;
        }

        for (Item* item : rest)
        {
            if (idx >= slots.size())
                break;

            targets.emplace_back(slots[idx], item->GetGUID());
            ++idx;
        }

        // Anchor quest items to the final slots of the pool (the last bag). All
        // items currently fit, so quest never overlaps the front-packed rest.
        if (!quest.empty())
        {
            std::size_t const qStart = slots.size() - quest.size();
            for (std::size_t q = 0; q < quest.size(); ++q)
                targets.emplace_back(slots[qStart + q], quest[q]->GetGUID());
        }

        return ApplyTargets(player, slots, targets);
    }
}

uint32 BagSorter::Sort(Player* player, SortMode mode)
{
    if (!player)
        return 0;

    std::vector<std::vector<uint16>> pools;
    BuildPools(player, pools);

    if (settings.MergeStacks)
        for (std::vector<uint16> const& pool : pools)
            ConsolidatePool(player, pool);

    // Hearthstone pinning and the quest sweep only apply to the general pool;
    // specialized bags just sort their own contents.
    SortMode const subMode = (mode == SortMode::TypeQualityQuestLast) ? SortMode::TypeQuality : mode;

    uint32 total = 0;
    for (std::size_t p = 0; p < pools.size(); ++p)
    {
        if (p == 0)
            total += PlaceGeneralPool(player, pools[p], mode);
        else
            total += PlacePool(player, pools[p], subMode);
    }

    return total;
}
