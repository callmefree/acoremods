// Pure-function unit tests for mod-terror-zones Slice 2 scaling math
// (plan §8.1). The live `TerrorZonesMgr::ComputeTargetLevel` wraps
// `ComputeTargetLevelPure` plus live session / pool lookups; the live
// `OnBeforeCreatureSelectLevel` hook calls `ApplyScaling` to enforce the
// "never scale down" invariant. This file covers both pure helpers.
//
// Eligibility predicate tests (pet / summon / boss / friendly / critter
// / blocklist) aren't here — they depend on Creature runtime state that
// can't be fabricated cheaply in a unit test. Those gates are covered
// by in-game force-tick verification (plan §8.5).

#include "TerrorZonesMgr.h"

#include <gtest/gtest.h>

using namespace mod_terror_zones;

// ---------- ComputeTargetLevelPure ----------
//
// New design: clamp(highestPlayerInZone, zone_levelMin, 80 + tier).
// Lets a level-80 player visit any TZ for tougher content (scaling
// to 80 + tier), while a low-level player in the same zone faces
// zone-level mobs. Mobs match the population, so loot can match too
// without breaking zone identity.

TEST(TerrorZonesScaling, NotEmpoweredReturnsZero)
{
    EXPECT_EQ(ComputeTargetLevelPure(/*empowered*/ false,
                                     /*highest*/ 80), 0);
    EXPECT_EQ(ComputeTargetLevelPure(false, 0), 0);
}

TEST(TerrorZonesScaling, EmpoweredHighestPlayerWithinRangeReturnsThat)
{
    // Westfall (zoneMin=10), T5 (ceiling 85), level-15 player → 15.
    EXPECT_EQ(ComputeTargetLevelPure(true, 15, /*min*/ 10, /*tier*/ 5), 15);
    // Same zone, level-30 leveling player → 30.
    EXPECT_EQ(ComputeTargetLevelPure(true, 30, 10, 5), 30);
    // Same zone, level-80 player → 80 (in range).
    EXPECT_EQ(ComputeTargetLevelPure(true, 80, 10, 5), 80);
}

TEST(TerrorZonesScaling, EmpoweredHighestPlayerCappedToCeiling)
{
    // Hypothetical post-cap player (GM cheat) lifts past 80 + tier.
    // T5 → ceiling = 85, so clamp(99, 10, 85) = 85.
    EXPECT_EQ(ComputeTargetLevelPure(true, 99, /*min*/ 10, /*tier*/ 5), 85);
    // T1 → ceiling = 81.
    EXPECT_EQ(ComputeTargetLevelPure(true, 99, 10, 1), 81);
    // T0 (default) → ceiling = 80.
    EXPECT_EQ(ComputeTargetLevelPure(true, 99), 80);
}

TEST(TerrorZonesScaling, EmpoweredLowestPlayerLiftedToZoneFloor)
{
    // Westfall (zoneMin=10), level-5 player wandering in → 10 (floor).
    EXPECT_EQ(ComputeTargetLevelPure(true, 5, /*min*/ 10, /*tier*/ 5), 10);
    // Icecrown (zoneMin=77), level-25 player → 77 (floor).
    EXPECT_EQ(ComputeTargetLevelPure(true, 25, 77, 5), 77);
    // No floor specified (default 0) → uses 1 to avoid level-0 mobs.
    EXPECT_EQ(ComputeTargetLevelPure(true, 0), 1);
}

// ---------- AggregatePlayerLevel ----------
//
// Zone-scoped target metric: the live ComputeTargetLevel collects the
// levels of real players standing in the empowered zone and folds them
// into one number via this helper (median by default, max optionally).
// Empty input → 0, which the live path treats as "no players → leave
// mobs native".

TEST(TerrorZonesScaling, AggregateEmptyReturnsZero)
{
    EXPECT_EQ(AggregatePlayerLevel({}, /*useMax*/ false), 0);
    EXPECT_EQ(AggregatePlayerLevel({}, /*useMax*/ true), 0);
}

TEST(TerrorZonesScaling, AggregateSinglePlayer)
{
    EXPECT_EQ(AggregatePlayerLevel({72}, false), 72);
    EXPECT_EQ(AggregatePlayerLevel({72}, true), 72);
}

TEST(TerrorZonesScaling, AggregateMedianOddCount)
{
    // Sorted {30, 60, 72} → middle is 60.
    EXPECT_EQ(AggregatePlayerLevel({72, 30, 60}, false), 60);
}

TEST(TerrorZonesScaling, AggregateMedianEvenCountLeansHigh)
{
    // Sorted {30, 72} → upper-middle (index n/2 = 1) is 72, so a
    // level-split duo targets the higher player rather than the lower.
    EXPECT_EQ(AggregatePlayerLevel({72, 30}, false), 72);
    // {20, 40, 60, 80} → index 2 = 60.
    EXPECT_EQ(AggregatePlayerLevel({80, 20, 60, 40}, false), 60);
}

TEST(TerrorZonesScaling, AggregateMaxIgnoresOthers)
{
    EXPECT_EQ(AggregatePlayerLevel({30, 72, 60}, /*useMax*/ true), 72);
}

// Composition: the zone-scoped target a level-72 solo player produces
// in an empowered Hinterlands (zoneMin=40, T1 → ceiling 81). The bug
// this fixes: pre-change the metric was server-wide and snapshotted at
// tick time, so an empty-at-tick zone floored to 40 and never lifted.
TEST(TerrorZonesScaling, AggregateThenTargetLevel72InLowZone)
{
    uint8 agg = AggregatePlayerLevel({72}, false);
    uint8 target = ComputeTargetLevelPure(true, agg, /*min*/ 40, /*tier*/ 1);
    EXPECT_EQ(target, 72);
    EXPECT_EQ(ApplyScaling(/*nativeRoll*/ 43, target), 72);
}

// ---------- ApplyScaling ----------

TEST(TerrorZonesScaling, ApplyScalingNoTargetReturnsBaseline)
{
    // target=0 means "not empowered" — baseline unchanged.
    EXPECT_EQ(ApplyScaling(/*baseline*/ 15, /*target*/ 0), 15);
    EXPECT_EQ(ApplyScaling(80, 0), 80);
}

TEST(TerrorZonesScaling, ApplyScalingNeverScalesDown)
{
    // A world-boss-adjacent elite already at level 82 in a zone empowered
    // to target 80 — keep it at 82. Scaling down would weaken an already-
    // strong creature.
    EXPECT_EQ(ApplyScaling(/*baseline*/ 82, /*target*/ 80), 82);
    // Same baseline as target — no change (no scaling up needed).
    EXPECT_EQ(ApplyScaling(80, 80), 80);
}

TEST(TerrorZonesScaling, ApplyScalingLifts)
{
    // Standard case: level-18 wolf in an 80-empowered zone → level 80.
    EXPECT_EQ(ApplyScaling(/*baseline*/ 18, /*target*/ 80), 80);
    EXPECT_EQ(ApplyScaling(1, 80), 80);
}

// ---------- Composition (what the hook actually does) ----------

TEST(TerrorZonesScaling, HookCompositionEmpoweredWestfallWithLevel80)
{
    // Level-80 real player logged in while Westfall is empowered — mobs
    // lift from level 18 to level 80.
    uint8 target = ComputeTargetLevelPure(/*empowered*/ true,
                                          /*highest*/ 80);
    uint8 baselineRoll = 18;
    uint8 finalLevel = ApplyScaling(baselineRoll, target);
    EXPECT_EQ(finalLevel, 80);
}

TEST(TerrorZonesScaling, HookCompositionNonEmpoweredZoneNoChange)
{
    uint8 target = ComputeTargetLevelPure(false, 80);
    uint8 finalLevel = ApplyScaling(/*baseline*/ 18, target);
    EXPECT_EQ(finalLevel, 18);
}

TEST(TerrorZonesScaling, HookCompositionFloorWhenServerEmpty)
{
    // Zone empowered but no real players logged in — mobs still run at
    // the retail level cap so the empowered zone remains endgame-ready
    // even during startup-resume walks (before any player logs in).
    uint8 target = ComputeTargetLevelPure(true, 0);
    uint8 finalLevel = ApplyScaling(15, target);
    EXPECT_EQ(finalLevel, 80);
}
