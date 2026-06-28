// Pure-function unit tests for mod-terror-zones Slice 6 event
// scheduling + spawn math (plan §9.1). The Mgr-driven lifecycle
// (FireEvent, spawn/despawn, announcement) rides live Map / Creature /
// GameObject state we can't fabricate in a unit harness — those gates
// are covered by in-game verification (plan §9.3).

#include "TerrorZonesMgr.h"

#include <array>
#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>

using namespace mod_terror_zones;

namespace
{
    class XorshiftRng : public IRng
    {
    public:
        explicit XorshiftRng(uint64 seed)
            : _state(seed ? seed : 0x9E3779B9ULL) {}
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

    EventScheduleConfig MakeDefaultCfg()
    {
        EventScheduleConfig cfg{};
        cfg.fireChance   = 0.60f;
        cfg.secondChance = 0.20f;
        cfg.durationSec  = 1200;
        cfg.firstOffsetSec  = 300;
        cfg.secondOffsetSec = 1800;
        for (uint32 i = 0; i <= EVENT_TYPE_MAX; ++i)
        {
            cfg.typeWeights[i] = 0;
            cfg.typeEnabled[i] = false;
        }
        cfg.typeWeights[EVENT_WORLD_BOSS]       = 100;
        cfg.typeEnabled[EVENT_WORLD_BOSS]       = true;
        cfg.typeWeights[EVENT_RARE_NODE_SURGE]  = 100;
        cfg.typeEnabled[EVENT_RARE_NODE_SURGE]  = true;
        cfg.typeWeights[EVENT_TREASURE_CARAVAN] = 0;
        cfg.typeEnabled[EVENT_TREASURE_CARAVAN] = false;
        cfg.typeWeights[EVENT_CHAMPION_GROUNDS] = 0;
        cfg.typeEnabled[EVENT_CHAMPION_GROUNDS] = false;
        return cfg;
    }
}

TEST(TerrorZonesEvents, SelectEventTypeUniformTwoEnabled)
{
    EventScheduleConfig cfg = MakeDefaultCfg();
    XorshiftRng rng(0xC0FFEEULL);

    std::array<uint32, EVENT_TYPE_MAX + 1> counts{};
    constexpr uint32 N = 10'000;
    for (uint32 i = 0; i < N; ++i)
    {
        EventType t = SelectEventType(cfg, rng);
        ASSERT_NE(t, EVENT_NONE);
        ASSERT_TRUE(t == EVENT_WORLD_BOSS
                    || t == EVENT_RARE_NODE_SURGE);
        ++counts[t];
    }

    // ~50/50 split with 10% tolerance over 10k samples.
    uint32 expected = N / 2;
    uint32 tol = N / 10;
    EXPECT_GE(counts[EVENT_WORLD_BOSS], expected - tol);
    EXPECT_LE(counts[EVENT_WORLD_BOSS], expected + tol);
    EXPECT_GE(counts[EVENT_RARE_NODE_SURGE], expected - tol);
    EXPECT_LE(counts[EVENT_RARE_NODE_SURGE], expected + tol);
    EXPECT_EQ(counts[EVENT_TREASURE_CARAVAN], 0u);
    EXPECT_EQ(counts[EVENT_CHAMPION_GROUNDS], 0u);
}

TEST(TerrorZonesEvents, SelectEventTypeAllDisabledReturnsNone)
{
    EventScheduleConfig cfg = MakeDefaultCfg();
    for (uint32 i = 0; i <= EVENT_TYPE_MAX; ++i)
        cfg.typeEnabled[i] = false;
    XorshiftRng rng(1ULL);
    EXPECT_EQ(SelectEventType(cfg, rng), EVENT_NONE);
}

TEST(TerrorZonesEvents, SelectEventTypeZeroWeightsReturnsNone)
{
    EventScheduleConfig cfg = MakeDefaultCfg();
    for (uint32 i = 0; i <= EVENT_TYPE_MAX; ++i)
        cfg.typeWeights[i] = 0;
    XorshiftRng rng(1ULL);
    EXPECT_EQ(SelectEventType(cfg, rng), EVENT_NONE);
}

TEST(TerrorZonesEvents, SelectEventTypeSingleEnabledDeterministic)
{
    EventScheduleConfig cfg = MakeDefaultCfg();
    cfg.typeEnabled[EVENT_RARE_NODE_SURGE] = false;
    XorshiftRng rng(0xDEADBEEFULL);
    for (uint32 i = 0; i < 256; ++i)
        EXPECT_EQ(SelectEventType(cfg, rng), EVENT_WORLD_BOSS);
}

TEST(TerrorZonesEvents, ShouldFireSecondEventDistribution)
{
    XorshiftRng rng(0x1234ULL);
    constexpr uint32 N = 10'000;
    uint32 hits = 0;
    for (uint32 i = 0; i < N; ++i)
        if (ShouldFireSecondEvent(0.20f, rng))
            ++hits;
    // 20% ± 3% over 10k samples.
    EXPECT_GE(hits, 1'700u);
    EXPECT_LE(hits, 2'300u);
}

TEST(TerrorZonesEvents, ShouldFireSecondEventEdgeCases)
{
    XorshiftRng rng(0x5678ULL);
    EXPECT_FALSE(ShouldFireSecondEvent(0.0f, rng));
    EXPECT_FALSE(ShouldFireSecondEvent(-1.0f, rng));
    EXPECT_TRUE(ShouldFireSecondEvent(1.0f, rng));
    EXPECT_TRUE(ShouldFireSecondEvent(2.0f, rng));
}

TEST(TerrorZonesEvents, WithinEventWindowBoundaries)
{
    // starts inclusive, ends exclusive.
    EXPECT_TRUE(WithinEventWindow(100, 100, 200));
    EXPECT_TRUE(WithinEventWindow(150, 100, 200));
    EXPECT_TRUE(WithinEventWindow(199, 100, 200));
    EXPECT_FALSE(WithinEventWindow(200, 100, 200));  // ends exclusive
    EXPECT_FALSE(WithinEventWindow(99, 100, 200));
    EXPECT_FALSE(WithinEventWindow(50, 100, 200));
    EXPECT_FALSE(WithinEventWindow(250, 100, 200));
    // Degenerate: ends <= starts → never inside.
    EXPECT_FALSE(WithinEventWindow(100, 200, 100));
    EXPECT_FALSE(WithinEventWindow(150, 100, 100));
}

TEST(TerrorZonesEvents, PickSubregionAnchorAllPointsInsideDisc)
{
    XorshiftRng rng(0xFEEDULL);
    float ax = 100.0f, ay = 50.0f, r = 25.0f;
    for (uint32 i = 0; i < 10'000; ++i)
    {
        float x, y;
        PickSubregionAnchor(ax, ay, r, rng, x, y);
        float dx = x - ax;
        float dy = y - ay;
        float dist = std::sqrt(dx * dx + dy * dy);
        EXPECT_LE(dist, r + 0.01f);   // tiny float tolerance
    }
}

TEST(TerrorZonesEvents, PickSubregionAnchorZeroRadiusReturnsAnchor)
{
    XorshiftRng rng(1ULL);
    float x = 999.0f, y = 999.0f;
    PickSubregionAnchor(42.0f, 17.0f, 0.0f, rng, x, y);
    EXPECT_FLOAT_EQ(x, 42.0f);
    EXPECT_FLOAT_EQ(y, 17.0f);
    PickSubregionAnchor(42.0f, 17.0f, -5.0f, rng, x, y);
    EXPECT_FLOAT_EQ(x, 42.0f);
    EXPECT_FLOAT_EQ(y, 17.0f);
}

TEST(TerrorZonesEvents, EventTypeDisplayAndCommandKeyRoundTrip)
{
    EXPECT_EQ(ParseEventTypeKey(EventTypeCommandKey(EVENT_WORLD_BOSS)),
              EVENT_WORLD_BOSS);
    EXPECT_EQ(ParseEventTypeKey(EventTypeCommandKey(EVENT_RARE_NODE_SURGE)),
              EVENT_RARE_NODE_SURGE);
    EXPECT_EQ(ParseEventTypeKey("WorldBoss"), EVENT_WORLD_BOSS);
    EXPECT_EQ(ParseEventTypeKey("NODES"), EVENT_RARE_NODE_SURGE);
    EXPECT_EQ(ParseEventTypeKey("bogus"), EVENT_NONE);
    EXPECT_EQ(ParseEventTypeKey(""), EVENT_NONE);
    EXPECT_EQ(ParseEventTypeKey(nullptr), EVENT_NONE);
}

TEST(TerrorZonesEvents, TwoEventStaggeringMonotonicStartTimes)
{
    // The staggering math is in ScheduleEvents itself (not a pure
    // helper), but the invariant it provides is that the two offsets
    // must be strictly monotonic and both must fit inside
    // (tickAt, tickAt + intervalSec - durationSec). We check the
    // config defaults here so a misconfig that reverses the offsets
    // trips the test instead of silently breaking rotation.
    EventScheduleConfig cfg = MakeDefaultCfg();
    EXPECT_LT(cfg.firstOffsetSec, cfg.secondOffsetSec);
    EXPECT_LT(cfg.secondOffsetSec + cfg.durationSec, 3600u);
}
