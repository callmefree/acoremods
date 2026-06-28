// Pure-function unit tests for Slice 8 combat-difficulty mults
// (plan §9.1). Covers ComputeCombatHpMult + ComputeCombatDamageMult
// composition semantics: baseline, per-tier bonus, event-boss
// uplift, and the <1.0 floor guard. Band lookup is covered by
// in-game verification (§9.3, step 5) — it's a deterministic linear
// scan against a live Mgr-loaded vector.

#include "TerrorZonesMgr.h"

#include <gtest/gtest.h>

using namespace mod_terror_zones;

namespace
{
    // Default-shape tier bonus ladder matching the Slice 8 conf
    // defaults. Indexed 0..TIER_MAX; index 0 is the TIER_NONE
    // sentinel (value 1.0 so ComputeCombatHpMult treats it as a
    // no-op and returns baseline unchanged).
    constexpr float kHpBonus[TIER_MAX + 1] = {
        1.00f, 1.00f, 1.25f, 1.50f, 1.75f, 2.00f
    };
    constexpr float kDamageBonusFlat[TIER_MAX + 1] = {
        1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f
    };
}

TEST(TerrorZonesCombatMult, HpBaselineNoTierNoEventBoss)
{
    EXPECT_FLOAT_EQ(ComputeCombatHpMult(2.0f, TIER_NONE, kHpBonus,
                                         /*isEventBoss*/false, 4.0f),
                    2.0f);
}

TEST(TerrorZonesCombatMult, HpTierBonusCompoundsMultiplicatively)
{
    // T3 bonus = 1.5x on top of 2.0 baseline → 3.0x.
    EXPECT_FLOAT_EQ(ComputeCombatHpMult(2.0f, TIER_3, kHpBonus,
                                         false, 4.0f),
                    3.0f);
    // T5 bonus = 2.0x on top of 2.0 baseline → 4.0x.
    EXPECT_FLOAT_EQ(ComputeCombatHpMult(2.0f, TIER_5, kHpBonus,
                                         false, 4.0f),
                    4.0f);
}

TEST(TerrorZonesCombatMult, HpEventBossUpliftStacksOnTop)
{
    // T1 event boss: 2.0 * 1.0 * 4.0 = 8.0x.
    EXPECT_FLOAT_EQ(ComputeCombatHpMult(2.0f, TIER_1, kHpBonus,
                                         /*isEventBoss*/true, 4.0f),
                    8.0f);
    // T3 event boss: 2.0 * 1.5 * 4.0 = 12.0x.
    EXPECT_FLOAT_EQ(ComputeCombatHpMult(2.0f, TIER_3, kHpBonus,
                                         true, 4.0f),
                    12.0f);
    // T5 event boss: 2.0 * 2.0 * 4.0 = 16.0x — the design ceiling.
    EXPECT_FLOAT_EQ(ComputeCombatHpMult(2.0f, TIER_5, kHpBonus,
                                         true, 4.0f),
                    16.0f);
}

TEST(TerrorZonesCombatMult, HpFloorsAtOne)
{
    // Misconfigured baseline of 0.5 shouldn't make empowered mobs
    // SOFTER than native — the composition floors at 1.0.
    EXPECT_FLOAT_EQ(ComputeCombatHpMult(0.5f, TIER_NONE, kHpBonus,
                                         false, 4.0f),
                    1.0f);
    // Tier bonus can't rescue a baseline that's still multiplying
    // below 1.0 until T5.
    EXPECT_FLOAT_EQ(ComputeCombatHpMult(0.4f, TIER_2, kHpBonus,
                                         false, 4.0f),
                    1.0f);  // 0.4 * 1.25 = 0.5, floored
    // But a large enough tier bonus will push above 1.0.
    EXPECT_FLOAT_EQ(ComputeCombatHpMult(0.6f, TIER_5, kHpBonus,
                                         false, 4.0f),
                    1.2f);  // 0.6 * 2.0 = 1.2, above floor
}

TEST(TerrorZonesCombatMult, DamageDefaultsFlatPerTier)
{
    // The default-conf damage-tier ladder is all-1.0 per plan §2.2.
    // A T5 mob hits at the same rate as a T1 mob.
    float t1 = ComputeCombatDamageMult(1.3f, TIER_1, kDamageBonusFlat,
                                        false, 1.75f);
    float t5 = ComputeCombatDamageMult(1.3f, TIER_5, kDamageBonusFlat,
                                        false, 1.75f);
    EXPECT_FLOAT_EQ(t1, 1.3f);
    EXPECT_FLOAT_EQ(t5, 1.3f);
}

TEST(TerrorZonesCombatMult, DamageEventBossUpliftStacks)
{
    // T{any} event boss: 1.3 * 1.0 * 1.75 = 2.275x. The "event boss
    // hits ~2.3x harder than native" line in the plan.
    EXPECT_NEAR(ComputeCombatDamageMult(1.3f, TIER_3, kDamageBonusFlat,
                                         true, 1.75f),
                2.275f, 1e-5f);
}

TEST(TerrorZonesCombatMult, CustomDamageTierLadderHonored)
{
    // If operator opts into tier-scales-damage by tuning the conf,
    // the math supports it — same shape as HP.
    float custom[TIER_MAX + 1] = {
        1.0f, 1.0f, 1.1f, 1.2f, 1.3f, 1.5f
    };
    EXPECT_FLOAT_EQ(ComputeCombatDamageMult(1.3f, TIER_5, custom,
                                             false, 1.75f),
                    1.3f * 1.5f);
}

TEST(TerrorZonesCombatMult, TierOutOfRangeTreatedAsNone)
{
    // Defensive — a tier value > TIER_MAX (shouldn't happen but let's
    // not crash) degrades to the TIER_NONE / no-bonus path.
    Tier bogus = static_cast<Tier>(TIER_MAX + 1);
    EXPECT_FLOAT_EQ(ComputeCombatHpMult(2.0f, bogus, kHpBonus,
                                         false, 4.0f),
                    2.0f);
}

// -----------------------------------------------------------------------------
// Slice 8b — elite-density promotion + composition
// -----------------------------------------------------------------------------

TEST(TerrorZonesCombatMult, ElitePromotionStacksOnTierBonus)
{
    // T3 promoted: 2.0 base × 1.5 tier × 1.5 elite = 4.5x.
    EXPECT_FLOAT_EQ(ComputeCombatHpMult(2.0f, TIER_3, kHpBonus,
                                         /*isEventBoss*/false, 4.0f,
                                         /*isPromotedElite*/true, 1.5f),
                    4.5f);
    // T5 promoted: 2.0 × 2.0 × 1.5 = 6.0x.
    EXPECT_FLOAT_EQ(ComputeCombatHpMult(2.0f, TIER_5, kHpBonus,
                                         false, 4.0f, true, 1.5f),
                    6.0f);
}

TEST(TerrorZonesCombatMult, ElitePromotionDamageStacks)
{
    // T3 promoted damage: 1.3 × 1.0 × 1.3 = 1.69x.
    EXPECT_NEAR(ComputeCombatDamageMult(1.3f, TIER_3, kDamageBonusFlat,
                                         false, 1.75f, true, 1.3f),
                1.69f, 1e-5f);
}

TEST(TerrorZonesCombatMult, ElitePromotionStacksWithEventBoss)
{
    // Event boss + promoted T3: 2.0 × 1.5 × 1.5 elite × 4.0 event = 18.0x.
    // (Event bosses promoted to elite is a degenerate case — a worldboss is
    // already the bossiest thing on the field — but the math should still
    // compose without surprise.)
    EXPECT_FLOAT_EQ(ComputeCombatHpMult(2.0f, TIER_3, kHpBonus,
                                         true, 4.0f, true, 1.5f),
                    18.0f);
}

TEST(TerrorZonesCombatMult, IsPromotedSpawnZeroThresholdNeverPromotes)
{
    // T1/T2 default density = 0 → no creature ever promotes.
    for (uint64 guid = 1; guid < 100; ++guid)
        EXPECT_FALSE(IsPromotedSpawn(guid, 1700000000ULL, 0));
}

TEST(TerrorZonesCombatMult, IsPromotedSpawnFullThresholdAlwaysPromotes)
{
    // Threshold 1000 = 100% — every spawn promotes.
    for (uint64 guid = 1; guid < 100; ++guid)
        EXPECT_TRUE(IsPromotedSpawn(guid, 1700000000ULL, 1000));
}

TEST(TerrorZonesCombatMult, IsPromotedSpawnDeterministicWithinRotation)
{
    // Same (guid, tickAt) → same answer. Caller relies on this so a
    // creature's promotion stays stable across rescale walks within a
    // rotation.
    constexpr uint64 tickAt = 1700000000ULL;
    constexpr uint32 threshold = 250;  // T4 default 25%
    for (uint64 guid = 1; guid < 200; ++guid)
    {
        bool a = IsPromotedSpawn(guid, tickAt, threshold);
        bool b = IsPromotedSpawn(guid, tickAt, threshold);
        bool c = IsPromotedSpawn(guid, tickAt, threshold);
        EXPECT_EQ(a, b);
        EXPECT_EQ(b, c);
    }
}

TEST(TerrorZonesCombatMult, IsPromotedSpawnDistributionApproximatesThreshold)
{
    // Sample 10000 guids at threshold=400 (T5 default). The promoted set
    // size should land within ±5pp of the threshold — Mix64 is well-mixed
    // enough that 400/1000 should produce ~3500-4500 promotions.
    constexpr uint64 tickAt = 1700000000ULL;
    uint32 promoted = 0;
    for (uint64 guid = 1; guid <= 10000; ++guid)
        if (IsPromotedSpawn(guid, tickAt, 400))
            ++promoted;
    EXPECT_GT(promoted, 3500u);
    EXPECT_LT(promoted, 4500u);
}

TEST(TerrorZonesCombatMult, IsPromotedSpawnRerollsAcrossRotations)
{
    // A new tickAt should produce a meaningfully-different promoted set
    // for the same set of guids. Sample 1000 guids at two different
    // tickAts and require at least 25% disagreement (any reasonable
    // random function will produce far more, but 25% is a robust floor
    // against accidental periodicity bugs).
    constexpr uint32 threshold = 400;
    uint32 disagree = 0;
    for (uint64 guid = 1; guid <= 1000; ++guid)
    {
        bool a = IsPromotedSpawn(guid, 1700000000ULL, threshold);
        bool b = IsPromotedSpawn(guid, 1700003600ULL, threshold);
        if (a != b)
            ++disagree;
    }
    EXPECT_GT(disagree, 250u);
}
