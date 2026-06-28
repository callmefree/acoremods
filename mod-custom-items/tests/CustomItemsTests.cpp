// Unit tests for the pure pieces of the custom-item pipeline. The
// DB-touching loader (`LoadCustomItems`) and the AllItemScript egress
// hook are exercised by in-game GM verification. Here we pin only the
// entry-range guard and the rewrite math, so a future regression that
// breaks either surface gets caught at the pre-merge gate.

#include "CustomItems.h"

#include <gtest/gtest.h>

namespace ci = mod_custom_items;

TEST(CustomItems_Range, BoundariesAreHalfOpen)
{
    // [700000, 800000) — inclusive on the floor, exclusive on the ceiling.
    EXPECT_FALSE(ci::IsCustomItemEntry(699'999u));
    EXPECT_TRUE (ci::IsCustomItemEntry(ci::kCustomItemEntryFloor));
    EXPECT_TRUE (ci::IsCustomItemEntry(700'001u));
    EXPECT_TRUE (ci::IsCustomItemEntry(799'999u));
    EXPECT_FALSE(ci::IsCustomItemEntry(ci::kCustomItemEntryCeil));
    EXPECT_FALSE(ci::IsCustomItemEntry(800'001u));

    // Real AC stock max (~56806) and zero must both fall outside.
    EXPECT_FALSE(ci::IsCustomItemEntry(0u));
    EXPECT_FALSE(ci::IsCustomItemEntry(56'806u));
}

TEST(CustomItems_Rewrite, OutOfRangePassesThrough)
{
    std::unordered_map<uint32, uint32> donors{{700'000u, 2080u}};
    EXPECT_EQ(ci::EvaluateRewriteEntry(0u, donors), 0u);
    EXPECT_EQ(ci::EvaluateRewriteEntry(2080u, donors), 2080u);
    EXPECT_EQ(ci::EvaluateRewriteEntry(56'806u, donors), 56'806u);
    EXPECT_EQ(ci::EvaluateRewriteEntry(699'999u, donors), 699'999u);
    EXPECT_EQ(ci::EvaluateRewriteEntry(800'000u, donors), 800'000u);
}

TEST(CustomItems_Rewrite, InRangeMappedReturnsDonor)
{
    std::unordered_map<uint32, uint32> donors{
        {700'000u, 2080u},
        {750'001u, 18'269u},
        {799'999u, 5212u},
    };
    EXPECT_EQ(ci::EvaluateRewriteEntry(700'000u, donors), 2080u);
    EXPECT_EQ(ci::EvaluateRewriteEntry(750'001u, donors), 18'269u);
    EXPECT_EQ(ci::EvaluateRewriteEntry(799'999u, donors), 5212u);
}

TEST(CustomItems_Rewrite, InRangeUnmappedFailsOpen)
{
    // Custom entry inside our reserved window but missing from the
    // donors map: we let the raw entry through. The boot-time loader
    // logs a warning when this happens; the egress path stays
    // forgiving so a misconfiguration doesn't hard-break visibility.
    std::unordered_map<uint32, uint32> donors{{700'000u, 2080u}};
    EXPECT_EQ(ci::EvaluateRewriteEntry(700'500u, donors), 700'500u);
    EXPECT_EQ(ci::EvaluateRewriteEntry(799'998u, donors), 799'998u);
}

TEST(CustomItems_Rewrite, EmptyDonorsMapPassesThrough)
{
    std::unordered_map<uint32, uint32> donors;
    EXPECT_EQ(ci::EvaluateRewriteEntry(700'000u, donors), 700'000u);
    EXPECT_EQ(ci::EvaluateRewriteEntry(2080u, donors), 2080u);
}
