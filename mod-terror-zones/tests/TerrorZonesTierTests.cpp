// Pure-function unit tests for Slice 5 tier selection (plan §10.1).
// Only the pure SelectTier helper is exercised here — the rotation-side
// persistence, Apply* integration, and GM command paths all need live
// state and are covered by in-game verification (plan §10.4).

#include "TerrorZonesMgr.h"

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

using namespace mod_terror_zones;

namespace
{
    class XorshiftRng : public IRng
    {
    public:
        explicit XorshiftRng(uint64 seed) : _state(seed ? seed : 0x9E3779B9ULL) {}
        uint32 NextUInt(uint32 maxExclusive) override
        {
            if (maxExclusive == 0)
                return 0;
            _state ^= _state >> 12;
            _state ^= _state << 25;
            _state ^= _state >> 27;
            uint64 scaled = (_state * 0x2545F4914F6CDD1DULL) >> 32;
            return static_cast<uint32>(scaled % maxExclusive);
        }
    private:
        uint64 _state;
    };
}

TEST(TerrorZonesTier, UniformWeightsCoverAllFivePaths)
{
    uint32 weights[TIER_MAX] = {20, 20, 20, 20, 20};
    XorshiftRng rng(0xDEADBEEFULL);

    std::array<uint32, TIER_MAX> counts{};
    constexpr uint32 N = 10'000;
    for (uint32 i = 0; i < N; ++i)
    {
        Tier t = SelectTier(weights, rng);
        ASSERT_NE(t, TIER_NONE);
        ASSERT_GE(t, TIER_1);
        ASSERT_LE(t, TIER_5);
        ++counts[t - 1];
    }

    uint32 expected = N / TIER_MAX;
    uint32 tolerance = N / 10;   // 10% is plenty for N=10k
    for (uint32 c : counts)
    {
        EXPECT_GE(c, expected - tolerance);
        EXPECT_LE(c, expected + tolerance);
    }
}

TEST(TerrorZonesTier, DefaultRarityDistribution)
{
    // 40/30/20/8/2 — the spec's default rarity curve. Check each bucket
    // lands within ±3% of its expectation over 10k samples.
    uint32 weights[TIER_MAX] = {40, 30, 20, 8, 2};
    XorshiftRng rng(0xFACEFEEDULL);

    std::array<uint32, TIER_MAX> counts{};
    constexpr uint32 N = 10'000;
    for (uint32 i = 0; i < N; ++i)
        ++counts[SelectTier(weights, rng) - 1];

    uint32 expected[TIER_MAX] = {
        N * 40 / 100, N * 30 / 100, N * 20 / 100,
        N *  8 / 100, N *  2 / 100
    };
    uint32 tolerance = N * 3 / 100;  // 3% slack
    for (uint32 i = 0; i < TIER_MAX; ++i)
    {
        uint32 lo = (expected[i] > tolerance) ? expected[i] - tolerance : 0;
        uint32 hi = expected[i] + tolerance;
        EXPECT_GE(counts[i], lo) << "tier " << (i + 1);
        EXPECT_LE(counts[i], hi) << "tier " << (i + 1);
    }
}

TEST(TerrorZonesTier, ZeroWeightsReturnNone)
{
    uint32 weights[TIER_MAX] = {0, 0, 0, 0, 0};
    XorshiftRng rng(1);
    EXPECT_EQ(SelectTier(weights, rng), TIER_NONE);
}

TEST(TerrorZonesTier, SingleNonZeroWeightAlwaysReturnsThatTier)
{
    uint32 weights[TIER_MAX] = {0, 0, 0, 0, 100};   // T5 only
    XorshiftRng rng(42);
    for (uint32 i = 0; i < 100; ++i)
        EXPECT_EQ(SelectTier(weights, rng), TIER_5);
}

TEST(TerrorZonesTier, ScaledWeightsSameAsRawWeights)
{
    // 10/5/2/1/0 should produce the same distribution (within sampling
    // noise) as 100/50/20/10/0. Only the ratio matters.
    uint32 smallWeights[TIER_MAX]  = {10, 5, 2, 1, 0};
    uint32 largeWeights[TIER_MAX]  = {100, 50, 20, 10, 0};
    constexpr uint32 N = 10'000;

    std::array<uint32, TIER_MAX> smallCounts{}, largeCounts{};
    XorshiftRng smallRng(0x1234ULL);
    XorshiftRng largeRng(0x5678ULL);
    for (uint32 i = 0; i < N; ++i)
    {
        ++smallCounts[SelectTier(smallWeights, smallRng) - 1];
        ++largeCounts[SelectTier(largeWeights, largeRng) - 1];
    }
    EXPECT_EQ(smallCounts[4], 0u);
    EXPECT_EQ(largeCounts[4], 0u);

    uint32 tolerance = N / 10;
    for (uint32 i = 0; i < 4; ++i)
    {
        uint32 diff = (smallCounts[i] > largeCounts[i])
                    ? smallCounts[i] - largeCounts[i]
                    : largeCounts[i] - smallCounts[i];
        EXPECT_LE(diff, tolerance) << "tier " << (i + 1);
    }
}

TEST(TerrorZonesTier, DisplayNameRoundTripsAllTiers)
{
    EXPECT_STREQ(TierDisplayName(TIER_1), "Tier 1");
    EXPECT_STREQ(TierDisplayName(TIER_2), "Tier 2");
    EXPECT_STREQ(TierDisplayName(TIER_3), "Tier 3");
    EXPECT_STREQ(TierDisplayName(TIER_4), "Tier 4");
    EXPECT_STREQ(TierDisplayName(TIER_5), "Tier 5");
    // §4.5 read-time compat: pre-Slice-5 rows display as Tier 1.
    EXPECT_STREQ(TierDisplayName(TIER_NONE), "Tier 1");
}
