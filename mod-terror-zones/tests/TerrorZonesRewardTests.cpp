// Pure-function unit tests for mod-terror-zones Slice 3 reward math
// (plan §8.1). Only `ComputeMultipliedValue` is exercised here —
// `TryTierBump`, `ApplyXpMultiplier`, and the loot / quest hook paths
// all require live Player / Loot / ItemTemplateContainer state that
// can't be fabricated cheaply in a unit harness. Those gates are
// covered by in-game verification (plan §11 steps 3–6).

#include "TerrorZonesMgr.h"

#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

using namespace mod_terror_zones;

TEST(TerrorZonesRewards, ZeroBaselineReturnsZero)
{
    EXPECT_EQ(ComputeMultipliedValue(0, 1.5f), 0u);
    EXPECT_EQ(ComputeMultipliedValue(0, 0.0f), 0u);
    EXPECT_EQ(ComputeMultipliedValue(0, 1000.0f), 0u);
}

TEST(TerrorZonesRewards, UnityMultiplierLeavesValueUnchanged)
{
    EXPECT_EQ(ComputeMultipliedValue(100, 1.0f), 100u);
    EXPECT_EQ(ComputeMultipliedValue(123456, 1.0f), 123456u);
}

TEST(TerrorZonesRewards, DefaultPlus50Percent)
{
    // Spec §4.4 default: +50% XP / +50% gold. 100 XP → 150 XP.
    EXPECT_EQ(ComputeMultipliedValue(100, 1.5f), 150u);
    EXPECT_EQ(ComputeMultipliedValue(1000, 1.5f), 1500u);
    // Small values still scale deterministically (integer truncation
    // of 1 * 1.5 = 1.5 → 1, matching "floor").
    EXPECT_EQ(ComputeMultipliedValue(1, 1.5f), 1u);
    EXPECT_EQ(ComputeMultipliedValue(2, 1.5f), 3u);
}

TEST(TerrorZonesRewards, ZeroMultiplierZerosOutput)
{
    // A GM setting XpMultiplier=0 in config effectively disables XP in
    // empowered zones. Guarded at config load (clamp floor at 0) so the
    // helper never sees a negative.
    EXPECT_EQ(ComputeMultipliedValue(500, 0.0f), 0u);
}

TEST(TerrorZonesRewards, NegativeMultiplierTreatedAsZero)
{
    // Defensive: if a misconfigured float ever leaks through.
    EXPECT_EQ(ComputeMultipliedValue(500, -0.5f), 0u);
}

TEST(TerrorZonesRewards, LargeMultiplierSaturatesAtMax)
{
    // Flavors in Slice 4 could stack multipliers up aggressively; make
    // sure we saturate to UINT32_MAX rather than wrap.
    uint32 big = 1'000'000'000u;
    uint32 result = ComputeMultipliedValue(big, 100.0f);
    EXPECT_EQ(result, std::numeric_limits<uint32>::max());
}

TEST(TerrorZonesRewards, UintMaxTimesUnityStays)
{
    uint32 max = std::numeric_limits<uint32>::max();
    EXPECT_EQ(ComputeMultipliedValue(max, 1.0f), max);
}

TEST(TerrorZonesRewards, FractionalDownMultiplierTruncates)
{
    // 0.5× baseline — confirms truncation is consistent with integer math.
    EXPECT_EQ(ComputeMultipliedValue(100, 0.5f), 50u);
    EXPECT_EQ(ComputeMultipliedValue(101, 0.5f), 50u);  // 50.5 → 50
}
