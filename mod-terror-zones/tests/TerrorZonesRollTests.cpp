// Pure-function unit tests for Slice 5 axis-roll math (plan §10.2).
// Covers determinism, range clamping, signature/secondary/neutral bias
// composition, and hard-cap enforcement. All tests exercise the pure
// ComputeAxisRoll helper; the Apply* wiring is covered by in-game
// verification (plan §10.4).

#include "TerrorZonesMgr.h"

#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>

using namespace mod_terror_zones;

namespace
{
    TierRollConfig MakeDefaultCfg()
    {
        // Matches the spec v0.2 §4.4 defaults.
        TierRollConfig cfg{};
        struct AD { float base; float spread; };
        AD d[TIER_MAX][AXIS_COUNT] = {
            {{1.20f, 0.10f}, {1.20f, 0.10f}, {0.025f, 0.25f}, {1.15f, 0.10f}, {0.010f, 0.30f}},
            {{1.35f, 0.12f}, {1.35f, 0.12f}, {0.040f, 0.25f}, {1.30f, 0.12f}, {0.020f, 0.30f}},
            {{1.55f, 0.15f}, {1.55f, 0.15f}, {0.065f, 0.30f}, {1.50f, 0.15f}, {0.040f, 0.35f}},
            {{1.85f, 0.18f}, {1.85f, 0.18f}, {0.095f, 0.30f}, {1.75f, 0.18f}, {0.075f, 0.35f}},
            {{2.30f, 0.20f}, {2.30f, 0.20f}, {0.140f, 0.30f}, {2.10f, 0.20f}, {0.140f, 0.40f}},
        };
        for (uint32 t = 0; t < TIER_MAX; ++t)
            for (uint32 a = 0; a < AXIS_COUNT; ++a)
            {
                cfg.tierTable[t][a].base   = d[t][a].base;
                cfg.tierTable[t][a].spread = d[t][a].spread;
            }
        cfg.signatureFloorBump   = 0.15f;
        cfg.signatureCeilingBump = 0.30f;
        cfg.secondaryFloorBump   = 0.05f;
        cfg.axisCaps[AXIS_XP]        = 5.0f;
        cfg.axisCaps[AXIS_GOLD]      = 8.0f;
        cfg.axisCaps[AXIS_TIER_BUMP] = 0.50f;
        cfg.axisCaps[AXIS_GATHERING] = 4.0f;
        cfg.axisCaps[AXIS_UNIQUES]   = 0.50f;
        return cfg;
    }
}

TEST(TerrorZonesRoll, DeterministicAcrossRepeatedCalls)
{
    TierRollConfig cfg = MakeDefaultCfg();
    uint64 tickAt = 1776912000ULL;
    float first = ComputeAxisRoll(tickAt, 0, FLAVOR_BLOODBATH, TIER_3,
                                    AXIS_XP, cfg);
    for (int i = 0; i < 100; ++i)
    {
        float v = ComputeAxisRoll(tickAt, 0, FLAVOR_BLOODBATH, TIER_3,
                                   AXIS_XP, cfg);
        EXPECT_FLOAT_EQ(v, first) << "drifted at iter=" << i;
    }
}

TEST(TerrorZonesRoll, RollRangeNeutralAxisStaysInBracket)
{
    TierRollConfig cfg = MakeDefaultCfg();
    // Bloodbath's non-signature / non-secondary axis is GATHERING. Across
    // 10k seeds the roll must stay inside [base × (1-spread), base × (1+spread)].
    constexpr uint32 N = 10'000;
    float base   = cfg.tierTable[TIER_3 - 1][AXIS_GATHERING].base;
    float spread = cfg.tierTable[TIER_3 - 1][AXIS_GATHERING].spread;
    float expectedLo = base * (1.0f - spread);
    float expectedHi = base * (1.0f + spread);
    for (uint32 i = 0; i < N; ++i)
    {
        float v = ComputeAxisRoll(1776912000ULL + i, 0,
                                    FLAVOR_BLOODBATH, TIER_3,
                                    AXIS_GATHERING, cfg);
        EXPECT_GE(v, expectedLo - 1e-4f);
        EXPECT_LE(v, expectedHi + 1e-4f);
    }
}

TEST(TerrorZonesRoll, SignatureAxisUsesBiasedRange)
{
    TierRollConfig cfg = MakeDefaultCfg();
    // Bloodbath signature=XP. Tier 3 XP: base=1.55, spread=0.15.
    // Biased lo = 1.55*(1+0.15-0.15)=1.55, biased hi = 1.55*(1+0.30+0.15)=1.55*1.45=2.2475.
    constexpr uint32 N = 10'000;
    float minSeen = 1e9f, maxSeen = -1e9f;
    for (uint32 i = 0; i < N; ++i)
    {
        float v = ComputeAxisRoll(1776912000ULL + i, 0,
                                    FLAVOR_BLOODBATH, TIER_3,
                                    AXIS_XP, cfg);
        minSeen = std::min(minSeen, v);
        maxSeen = std::max(maxSeen, v);
    }
    EXPECT_NEAR(minSeen, 1.55f,   0.02f);   // generous slack — 10k samples won't hit exact endpoints
    EXPECT_NEAR(maxSeen, 2.2475f, 0.02f);
}

TEST(TerrorZonesRoll, SecondaryAxisFloorLiftsAboveNeutral)
{
    TierRollConfig cfg = MakeDefaultCfg();
    // Bloodbath secondary axes are Gold + TierBump. Gold at T3 neutral floor =
    // 1.55*0.85 = 1.3175; biased floor = 1.55*(1+0.05-0.15) = 1.55*0.90 = 1.395.
    constexpr uint32 N = 10'000;
    float minSecondary = 1e9f;
    float minNeutral   = 1e9f;
    for (uint32 i = 0; i < N; ++i)
    {
        float s = ComputeAxisRoll(9000000ULL + i, 0,
                                    FLAVOR_BLOODBATH, TIER_3,
                                    AXIS_GOLD, cfg);
        float n = ComputeAxisRoll(9000000ULL + i, 0,
                                    FLAVOR_ARCANE, TIER_3,
                                    AXIS_GOLD, cfg);
        minSecondary = std::min(minSecondary, s);
        minNeutral   = std::min(minNeutral,   n);
    }
    EXPECT_GT(minSecondary, minNeutral);
}

TEST(TerrorZonesRoll, HardCapClamps)
{
    TierRollConfig cfg = MakeDefaultCfg();
    // Synthesize a tier-axis entry with runaway base, confirm the roll
    // clamps at the per-axis cap regardless of how high the bracket
    // would otherwise reach.
    cfg.tierTable[TIER_5 - 1][AXIS_XP].base   = 50.0f;
    cfg.tierTable[TIER_5 - 1][AXIS_XP].spread = 0.01f;
    float rolled = ComputeAxisRoll(1776912000ULL, 0,
                                     FLAVOR_BLOODBATH, TIER_5,
                                     AXIS_XP, cfg);
    EXPECT_FLOAT_EQ(rolled, cfg.axisCaps[AXIS_XP]);
}

TEST(TerrorZonesRoll, FlavorBiasMakesSignatureDominate)
{
    TierRollConfig cfg = MakeDefaultCfg();
    // Statistical claim: over many seeds, Bloodbath's XP (signature)
    // rolls ABOVE Merchant's XP (neutral) on average at the same tier.
    constexpr uint32 N = 1000;
    double sumBloodbath = 0.0, sumMerchants = 0.0;
    for (uint32 i = 0; i < N; ++i)
    {
        sumBloodbath += ComputeAxisRoll(500000ULL + i, 0,
                                          FLAVOR_BLOODBATH, TIER_3,
                                          AXIS_XP, cfg);
        sumMerchants += ComputeAxisRoll(500000ULL + i, 0,
                                          FLAVOR_MERCHANTS, TIER_3,
                                          AXIS_XP, cfg);
    }
    double avgBloodbath = sumBloodbath / N;
    double avgMerchants = sumMerchants / N;
    EXPECT_GT(avgBloodbath, avgMerchants);
}

TEST(TerrorZonesRoll, TierNoneResolvesToTier1)
{
    TierRollConfig cfg = MakeDefaultCfg();
    // §4.5 compat — pre-Slice-5 rows (TIER_NONE) should read as Tier 1.
    for (uint32 i = 0; i < 100; ++i)
    {
        float vNone = ComputeAxisRoll(1000000ULL + i, 0,
                                        FLAVOR_BLOODBATH, TIER_NONE,
                                        AXIS_XP, cfg);
        float vOne  = ComputeAxisRoll(1000000ULL + i, 0,
                                        FLAVOR_BLOODBATH, TIER_1,
                                        AXIS_XP, cfg);
        EXPECT_FLOAT_EQ(vNone, vOne);
    }
}

TEST(TerrorZonesRoll, SlotIndexChangesRoll)
{
    TierRollConfig cfg = MakeDefaultCfg();
    // Same tuple except slotIndex — rolls should differ. Two slots in
    // the same rotation slot=0 vs slot=1 must not collide.
    float slot0 = ComputeAxisRoll(1776912000ULL, 0, FLAVOR_BLOODBATH,
                                    TIER_3, AXIS_XP, cfg);
    float slot1 = ComputeAxisRoll(1776912000ULL, 1, FLAVOR_BLOODBATH,
                                    TIER_3, AXIS_XP, cfg);
    EXPECT_NE(slot0, slot1);
}

TEST(TerrorZonesRoll, FlavorBiasOfArcaneSignatureIsUniques)
{
    // Lock in the v0.2 design call: Arcane signature axis = Uniques.
    FlavorBiasDef const& bias = FlavorBiasOf(FLAVOR_ARCANE);
    EXPECT_EQ(bias.primary, AXIS_UNIQUES);
}

TEST(TerrorZonesRoll, WorkedExampleTier4Merchants)
{
    TierRollConfig cfg = MakeDefaultCfg();
    // Spec §4.4 worked example: Tier 4 Merchant's Gold (signature) in
    // [1.85 × (1.15 - 0.18), 1.85 × (1.30 + 0.18)] = [1.79, 2.74].
    // Sample across many seeds and check endpoints.
    constexpr uint32 N = 20'000;
    float minSeen = 1e9f, maxSeen = -1e9f;
    for (uint32 i = 0; i < N; ++i)
    {
        float v = ComputeAxisRoll(2000000ULL + i, 0,
                                    FLAVOR_MERCHANTS, TIER_4,
                                    AXIS_GOLD, cfg);
        minSeen = std::min(minSeen, v);
        maxSeen = std::max(maxSeen, v);
    }
    EXPECT_NEAR(minSeen, 1.79f, 0.03f);
    EXPECT_NEAR(maxSeen, 2.74f, 0.03f);
}
