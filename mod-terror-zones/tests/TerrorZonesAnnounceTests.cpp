// Pure-function unit tests for Slice 7 announcement gating + lead-time
// predicates (plan §9.1). Covers IsCategoryAnnouncementAllowed
// composition, ShouldFireRotationEndingWarning + ShouldFireEventEndingCountdown
// boundary / dedupe / missed-window semantics, and the
// AnnounceCategoryKey / AnnounceCategoryAlias parse round-trip.

#include "TerrorZonesMgr.h"

#include <gtest/gtest.h>

using namespace mod_terror_zones;

// -----------------------------------------------------------------------------
// IsCategoryAnnouncementAllowed
// -----------------------------------------------------------------------------

TEST(TerrorZonesAnnounce, MasterOffSilencesAllCategories)
{
    EXPECT_FALSE(IsCategoryAnnouncementAllowed(
        ANNOUNCE_ROTATION_TICK, ANNOUNCE_CATEGORY_ALL,
        /*playerMasterOn*/false, ANNOUNCE_CATEGORY_ALL));
    EXPECT_FALSE(IsCategoryAnnouncementAllowed(
        ANNOUNCE_EVENT_END, ANNOUNCE_CATEGORY_ALL,
        false, ANNOUNCE_CATEGORY_ALL));
}

TEST(TerrorZonesAnnounce, GlobalBitOffSilencesCategory)
{
    uint8 globalNoEvents = ANNOUNCE_CATEGORY_ALL
        & ~AnnounceCategoryBit(ANNOUNCE_EVENT_START);
    EXPECT_FALSE(IsCategoryAnnouncementAllowed(
        ANNOUNCE_EVENT_START, globalNoEvents, true, ANNOUNCE_CATEGORY_ALL));
    EXPECT_TRUE(IsCategoryAnnouncementAllowed(
        ANNOUNCE_ROTATION_TICK, globalNoEvents, true, ANNOUNCE_CATEGORY_ALL));
}

TEST(TerrorZonesAnnounce, PlayerBitOffSilencesCategory)
{
    uint8 playerNoZoneEntry = ANNOUNCE_CATEGORY_ALL
        & ~AnnounceCategoryBit(ANNOUNCE_ZONE_ENTRY);
    EXPECT_FALSE(IsCategoryAnnouncementAllowed(
        ANNOUNCE_ZONE_ENTRY, ANNOUNCE_CATEGORY_ALL, true, playerNoZoneEntry));
    EXPECT_TRUE(IsCategoryAnnouncementAllowed(
        ANNOUNCE_ZONE_LEAVE, ANNOUNCE_CATEGORY_ALL, true, playerNoZoneEntry));
}

TEST(TerrorZonesAnnounce, AllOpenAllowsLine)
{
    EXPECT_TRUE(IsCategoryAnnouncementAllowed(
        ANNOUNCE_ROTATION_END, ANNOUNCE_CATEGORY_ALL, true,
        ANNOUNCE_CATEGORY_ALL));
}

TEST(TerrorZonesAnnounce, OutOfRangeCategoryIsRejected)
{
    auto bogus = static_cast<AnnounceCategory>(ANNOUNCE_CATEGORY_COUNT);
    EXPECT_FALSE(IsCategoryAnnouncementAllowed(
        bogus, ANNOUNCE_CATEGORY_ALL, true, ANNOUNCE_CATEGORY_ALL));
}

// -----------------------------------------------------------------------------
// ShouldFireRotationEndingWarning
// -----------------------------------------------------------------------------

TEST(TerrorZonesAnnounce, RotationEndingDisabledWhenLeadZero)
{
    EXPECT_FALSE(ShouldFireRotationEndingWarning(
        /*now*/1000, /*nextTickAt*/1300, /*leadSec*/0, /*windowSec*/30,
        /*lastWarnTickAt*/0));
}

TEST(TerrorZonesAnnounce, RotationEndingTooEarly)
{
    // now = 800, lead = 300, window = 30. Window is [1000, 1030].
    EXPECT_FALSE(ShouldFireRotationEndingWarning(
        800, 1300, 300, 30, 0));
    EXPECT_FALSE(ShouldFireRotationEndingWarning(
        999, 1300, 300, 30, 0));
}

TEST(TerrorZonesAnnounce, RotationEndingFiresAtBoundary)
{
    EXPECT_TRUE(ShouldFireRotationEndingWarning(
        1000, 1300, 300, 30, 0));
}

TEST(TerrorZonesAnnounce, RotationEndingFiresWithinSlackWindow)
{
    EXPECT_TRUE(ShouldFireRotationEndingWarning(
        1015, 1300, 300, 30, 0));
    EXPECT_TRUE(ShouldFireRotationEndingWarning(
        1030, 1300, 300, 30, 0));
}

TEST(TerrorZonesAnnounce, RotationEndingMissedWindowSuppressed)
{
    // Window closed at 1030 (1000 + 30). 1031 is too late.
    EXPECT_FALSE(ShouldFireRotationEndingWarning(
        1031, 1300, 300, 30, 0));
    // Even past the tick itself — no late-fire after restart.
    EXPECT_FALSE(ShouldFireRotationEndingWarning(
        1400, 1300, 300, 30, 0));
}

TEST(TerrorZonesAnnounce, RotationEndingDedupedByLastWarn)
{
    // Already fired for nextTickAt=1300.
    EXPECT_FALSE(ShouldFireRotationEndingWarning(
        1015, 1300, 300, 30, /*lastWarnTickAt*/1300));
    // Not fired yet for the *next* rotation (nextTickAt=2900).
    EXPECT_TRUE(ShouldFireRotationEndingWarning(
        2600, 2900, 300, 30, 1300));
}

TEST(TerrorZonesAnnounce, RotationEndingNextTickShorterThanLeadIsRejected)
{
    // Pathological: nextTickAt < leadSec. Should silently bail (uint64
    // underflow guard).
    EXPECT_FALSE(ShouldFireRotationEndingWarning(
        100, 200, 300, 30, 0));
}

// -----------------------------------------------------------------------------
// ShouldFireEventEndingCountdown
// -----------------------------------------------------------------------------

TEST(TerrorZonesAnnounce, EventCountdownDisabledWhenLeadZero)
{
    EXPECT_FALSE(ShouldFireEventEndingCountdown(
        /*now*/1000, /*endsAt*/1300, /*leadSec*/0, /*windowSec*/30,
        /*alreadyFired*/false));
}

TEST(TerrorZonesAnnounce, EventCountdownDedupedByAlreadyFired)
{
    EXPECT_FALSE(ShouldFireEventEndingCountdown(
        1015, 1300, 300, 30, /*alreadyFired*/true));
}

TEST(TerrorZonesAnnounce, EventCountdownFiresAtBoundary)
{
    EXPECT_TRUE(ShouldFireEventEndingCountdown(
        1000, 1300, 300, 30, false));
    EXPECT_TRUE(ShouldFireEventEndingCountdown(
        1030, 1300, 300, 30, false));
}

TEST(TerrorZonesAnnounce, EventCountdownMissedWindowSuppressed)
{
    EXPECT_FALSE(ShouldFireEventEndingCountdown(
        1031, 1300, 300, 30, false));
    EXPECT_FALSE(ShouldFireEventEndingCountdown(
        1290, 1300, 300, 30, false));
}

TEST(TerrorZonesAnnounce, EventCountdownEndsAtShorterThanLeadIsRejected)
{
    EXPECT_FALSE(ShouldFireEventEndingCountdown(
        100, 200, 300, 30, false));
}

// -----------------------------------------------------------------------------
// Category key / alias parsing
// -----------------------------------------------------------------------------

TEST(TerrorZonesAnnounce, CategoryKeyRoundTrip)
{
    for (uint8 i = 0; i < ANNOUNCE_CATEGORY_COUNT; ++i)
    {
        auto cat = static_cast<AnnounceCategory>(i);
        char const* key = AnnounceCategoryCommandKey(cat);
        EXPECT_EQ(ParseAnnounceCategoryKey(key), cat);
    }
}

TEST(TerrorZonesAnnounce, CategoryKeyUnknownReturnsCount)
{
    EXPECT_EQ(ParseAnnounceCategoryKey("nonsense"),
              ANNOUNCE_CATEGORY_COUNT);
    EXPECT_EQ(ParseAnnounceCategoryKey(nullptr),
              ANNOUNCE_CATEGORY_COUNT);
}

TEST(TerrorZonesAnnounce, AliasEventCoversAllThreeEventBits)
{
    uint8 mask = ParseAnnounceCategoryAlias("event");
    EXPECT_TRUE(mask & AnnounceCategoryBit(ANNOUNCE_EVENT_START));
    EXPECT_TRUE(mask & AnnounceCategoryBit(ANNOUNCE_EVENT_ENDING));
    EXPECT_TRUE(mask & AnnounceCategoryBit(ANNOUNCE_EVENT_END));
    // Doesn't leak into rotation/zone bits.
    EXPECT_FALSE(mask & AnnounceCategoryBit(ANNOUNCE_ROTATION_TICK));
    EXPECT_FALSE(mask & AnnounceCategoryBit(ANNOUNCE_ZONE_ENTRY));
    // Plural "events" works too.
    EXPECT_EQ(ParseAnnounceCategoryAlias("events"), mask);
}

TEST(TerrorZonesAnnounce, AliasAllReturnsAll)
{
    EXPECT_EQ(ParseAnnounceCategoryAlias("all"), ANNOUNCE_CATEGORY_ALL);
}

TEST(TerrorZonesAnnounce, AliasZoneCoversBothZoneBits)
{
    uint8 mask = ParseAnnounceCategoryAlias("zone");
    EXPECT_TRUE(mask & AnnounceCategoryBit(ANNOUNCE_ZONE_ENTRY));
    EXPECT_TRUE(mask & AnnounceCategoryBit(ANNOUNCE_ZONE_LEAVE));
    EXPECT_FALSE(mask & AnnounceCategoryBit(ANNOUNCE_EVENT_END));
}

TEST(TerrorZonesAnnounce, AliasUnknownReturnsZero)
{
    EXPECT_EQ(ParseAnnounceCategoryAlias("frob"), 0);
    EXPECT_EQ(ParseAnnounceCategoryAlias(nullptr), 0);
}

TEST(TerrorZonesAnnounce, BitmaskRoundTripStableAcrossPatterns)
{
    // Sanity: bit positions don't overlap and cover [0, 7].
    uint8 acc = 0;
    for (uint8 i = 0; i < ANNOUNCE_CATEGORY_COUNT; ++i)
    {
        uint8 bit = AnnounceCategoryBit(static_cast<AnnounceCategory>(i));
        EXPECT_EQ(acc & bit, 0);  // no overlap
        acc |= bit;
    }
    EXPECT_EQ(acc, ANNOUNCE_CATEGORY_ALL);
}
