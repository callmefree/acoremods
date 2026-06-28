// Pure-function unit tests for mod-terror-zones Slice 4 flavor selection
// (plan §11.1). Only the pure SelectFlavor helper is exercised here —
// the Apply* overlay paths, atmosphere override, gathering hook, and
// unique-drop roll all require live Player / Map / Loot / ItemTemplate
// state that can't be fabricated cheaply in a unit harness. Those gates
// are covered by in-game verification (plan §11.4).

#include "TerrorZonesMgr.h"

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

using namespace mod_terror_zones;

namespace
{
    // Deterministic RNG (xorshift64*) so distribution tests are repeatable.
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

    // Replay an explicit sequence — makes it trivial to prove that a
    // given roll lands on a specific flavor.
    class ScriptedRng : public IRng
    {
    public:
        ScriptedRng(std::initializer_list<uint32> rolls)
            : _rolls(rolls), _idx(0) {}
        uint32 NextUInt(uint32 maxExclusive) override
        {
            uint32 r = (_idx < _rolls.size()) ? _rolls[_idx++] : 0;
            return (maxExclusive == 0) ? 0 : r % maxExclusive;
        }
    private:
        std::vector<uint32> _rolls;
        size_t _idx;
    };
}

TEST(TerrorZonesFlavor, UniformWeightsCoverAllFivePaths)
{
    uint32 weights[FLAVOR_MAX] = {100, 100, 100, 100, 100};
    XorshiftRng rng(0xC0FFEEULL);

    std::array<uint32, FLAVOR_MAX> counts{};
    constexpr uint32 N = 10'000;
    for (uint32 i = 0; i < N; ++i)
    {
        Flavor f = SelectFlavor(weights, rng);
        ASSERT_NE(f, FLAVOR_NONE);
        ASSERT_GE(f, FLAVOR_BLOODBATH);
        ASSERT_LE(f, FLAVOR_MERCHANTS);
        ++counts[f - 1];
    }

    // Expect ~20% each with uniform weights. 10% tolerance is generous;
    // typical spread for N=10k is under 2%.
    uint32 expected = N / FLAVOR_MAX;
    uint32 tolerance = N / 10;
    for (uint32 c : counts)
    {
        EXPECT_GE(c, expected - tolerance);
        EXPECT_LE(c, expected + tolerance);
    }
}

TEST(TerrorZonesFlavor, ZeroWeightsReturnNone)
{
    uint32 weights[FLAVOR_MAX] = {0, 0, 0, 0, 0};
    XorshiftRng rng(42);
    EXPECT_EQ(SelectFlavor(weights, rng), FLAVOR_NONE);
}

TEST(TerrorZonesFlavor, SingleNonZeroWeightAlwaysReturnsThatFlavor)
{
    uint32 weights[FLAVOR_MAX] = {0, 0, 100, 0, 0};   // Warlord's only
    XorshiftRng rng(1);
    for (uint32 i = 0; i < 100; ++i)
        EXPECT_EQ(SelectFlavor(weights, rng), FLAVOR_WARLORDS);
}

TEST(TerrorZonesFlavor, ScriptedRollFirstBucket)
{
    // Weights [10, 20, 30, 40, 50] → total 150, buckets end at
    // 10/30/60/100/150. Roll=5 → first bucket (Bloodbath).
    uint32 weights[FLAVOR_MAX] = {10, 20, 30, 40, 50};
    ScriptedRng rng({5});
    EXPECT_EQ(SelectFlavor(weights, rng), FLAVOR_BLOODBATH);
}

TEST(TerrorZonesFlavor, ScriptedRollLastBucket)
{
    // Same weights; roll=149 (just inside the last bucket) → Merchants.
    uint32 weights[FLAVOR_MAX] = {10, 20, 30, 40, 50};
    ScriptedRng rng({149});
    EXPECT_EQ(SelectFlavor(weights, rng), FLAVOR_MERCHANTS);
}

TEST(TerrorZonesFlavor, DisplayNameRoundTripsAllFlavors)
{
    // Smoke — catch accidental enum/display desync.
    EXPECT_STREQ(FlavorDisplayName(FLAVOR_BLOODBATH),   "Bloodbath");
    EXPECT_STREQ(FlavorDisplayName(FLAVOR_PROSPECTORS), "Prospector's");
    EXPECT_STREQ(FlavorDisplayName(FLAVOR_WARLORDS),    "Warlord's");
    EXPECT_STREQ(FlavorDisplayName(FLAVOR_ARCANE),      "Arcane");
    EXPECT_STREQ(FlavorDisplayName(FLAVOR_MERCHANTS),   "Merchant's");
    EXPECT_STREQ(FlavorDisplayName(FLAVOR_NONE),        "—");
}

TEST(TerrorZonesFlavor, CommandKeyIsLowercaseAscii)
{
    // The `.zones testflavor <key>` parser expects lowercase keys.
    EXPECT_STREQ(FlavorCommandKey(FLAVOR_BLOODBATH),   "bloodbath");
    EXPECT_STREQ(FlavorCommandKey(FLAVOR_PROSPECTORS), "prospectors");
    EXPECT_STREQ(FlavorCommandKey(FLAVOR_WARLORDS),    "warlords");
    EXPECT_STREQ(FlavorCommandKey(FLAVOR_ARCANE),      "arcane");
    EXPECT_STREQ(FlavorCommandKey(FLAVOR_MERCHANTS),   "merchants");
}

TEST(TerrorZonesFlavor, OverlayMathComposesWithSlice3Helper)
{
    // ComputeMultipliedValue(base, Rewards.XpMultiplier * flavor.XpBoost)
    // must match ComputeMultipliedValue(ComputeMultipliedValue(base,
    // Rewards.XpMultiplier), flavor.XpBoost) within float tolerance.
    // This proves the overlay can live as an in-line multiplication
    // without needing its own helper.
    uint32 base = 1000;
    float rewardMult = 1.5f;
    float flavorBoost = 1.5f;  // Bloodbath

    uint32 composed = ComputeMultipliedValue(base, rewardMult * flavorBoost);
    uint32 staged = ComputeMultipliedValue(
        ComputeMultipliedValue(base, rewardMult), flavorBoost);

    // Due to float intermediate rounding, allow ±1 ulp drift.
    uint32 diff = (composed > staged) ? (composed - staged) : (staged - composed);
    EXPECT_LE(diff, 1u) << "composed=" << composed << " staged=" << staged;
}
