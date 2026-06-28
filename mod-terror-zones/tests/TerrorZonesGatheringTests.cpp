// Pure-function unit tests for mod-terror-zones Slice 4 gathering-store
// detection (plan §11.2). The live TryGatheringYieldBump path needs
// Player / Loot state; this file exercises only the name-matching
// predicate, which is the piece most likely to silently rot if AC
// renames a LootStore in the future.

#include "TerrorZonesMgr.h"

#include <gtest/gtest.h>

using namespace mod_terror_zones;

TEST(TerrorZonesGathering, GameObjectStoreIsGathering)
{
    EXPECT_TRUE(IsGatheringStore("gameobject_loot_template"));
}

TEST(TerrorZonesGathering, SkinningStoreIsGathering)
{
    EXPECT_TRUE(IsGatheringStore("skinning_loot_template"));
}

TEST(TerrorZonesGathering, CreatureStoreIsNotGathering)
{
    // Mob kill loot is not gathering — Prospector's bump must not fire
    // when killing a wolf.
    EXPECT_FALSE(IsGatheringStore("creature_loot_template"));
}

TEST(TerrorZonesGathering, ReferenceStoreIsNotGathering)
{
    EXPECT_FALSE(IsGatheringStore("reference_loot_template"));
}

TEST(TerrorZonesGathering, DisenchantStoreIsNotGathering)
{
    // Disenchanting a blue doesn't count as gathering for flavor
    // purposes — it's a different skill loop.
    EXPECT_FALSE(IsGatheringStore("disenchant_loot_template"));
}

TEST(TerrorZonesGathering, FishingStoreIsNotGathering)
{
    // Spec calls "gathering", fishing has its own rhythm — plan §9.1
    // intentionally excludes it. If this changes later, flip this test
    // and update the plan.
    EXPECT_FALSE(IsGatheringStore("fishing_loot_template"));
}

TEST(TerrorZonesGathering, EmptyStringIsNotGathering)
{
    EXPECT_FALSE(IsGatheringStore(""));
}

TEST(TerrorZonesGathering, NullPointerIsNotGathering)
{
    // Defensive — LootStore::GetName() should never return null in
    // current AC, but the helper handles it gracefully.
    EXPECT_FALSE(IsGatheringStore(nullptr));
}
