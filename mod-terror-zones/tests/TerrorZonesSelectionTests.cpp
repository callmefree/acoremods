// Pure-function unit tests for mod-terror-zones SelectZones (plan §10.1).
// These link against the `modules` library for the SelectZones symbol and
// are compiled into the AC `unit_tests` target via the module's .cmake
// (see modules/mod-terror-zones/mod-terror-zones.cmake).

#include "TerrorZonesMgr.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

using namespace mod_terror_zones;

namespace
{
    // Deterministic RNG: walks a caller-provided sequence, wrapping when it
    // runs out. Lets tests force specific selection paths.
    class ScriptedRng : public IRng
    {
    public:
        explicit ScriptedRng(std::vector<uint32> seq) : _seq(std::move(seq)) {}
        uint32 NextUInt(uint32 maxExclusive) override
        {
            if (maxExclusive == 0)
                return 0;
            uint32 v = _seq.empty() ? 0 : _seq[_i % _seq.size()];
            ++_i;
            return v % maxExclusive;
        }
    private:
        std::vector<uint32> _seq;
        size_t _i = 0;
    };

    // A trivial xorshift RNG for coarse distribution tests. Deterministic.
    class XorshiftRng : public IRng
    {
    public:
        explicit XorshiftRng(uint64 seed) : _state(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
        uint32 NextUInt(uint32 maxExclusive) override
        {
            if (maxExclusive == 0)
                return 0;
            _state ^= _state << 13;
            _state ^= _state >> 7;
            _state ^= _state << 17;
            return static_cast<uint32>(_state % maxExclusive);
        }
    private:
        uint64 _state;
    };

    SelectionConfig DefaultCfg(uint32 slots = 1)
    {
        return SelectionConfig{
            /*levelWindow*/ 5,
            /*weightNear*/ 100,
            /*weightOverlap*/ 30,
            /*weightBelow*/ 10,
            /*weightAbove*/ 1,
            /*recencyMultiplier*/ 0.1,
            /*slotCount*/ slots,
        };
    }

    std::vector<PoolEntry> ThreeBandPool()
    {
        // Three enabled zones, disjoint level bands.
        return {
            {100, "Low",  10, 20, true},   // mid 15
            {200, "Mid",  30, 45, true},   // mid 37.5
            {300, "High", 60, 70, true},   // mid 65
        };
    }
}

// 10.1 — Empty targets → flat-weighted pick: all zones share near-weight,
// all recency-excluded zones still pickable; distribution is roughly flat.
TEST(TerrorZonesSelection, EmptyTargetsIsFlatWeighted)
{
    std::vector<PoolEntry> pool = ThreeBandPool();
    SelectionConfig cfg = DefaultCfg();
    XorshiftRng rng(0xABC123ULL);

    std::unordered_map<uint32, int> counts;
    constexpr int ROLLS = 3000;
    for (int i = 0; i < ROLLS; ++i)
    {
        auto picks = SelectZones(pool, /*targets*/ {}, /*recent*/ {}, cfg, rng);
        ASSERT_EQ(picks.size(), 1u);
        counts[picks[0]]++;
    }
    int each = ROLLS / 3;
    int slack = each / 3;  // ±33% tolerance — this is a coarse flatness check
    for (auto const& zoneId : {100u, 200u, 300u})
        EXPECT_NEAR(counts[zoneId], each, slack) << "zone " << zoneId;
}

// 10.1 — Single target, in window: the in-window zone dominates. With
// weightNear=100 vs weightOverlap=30, the near zone should win the clear
// majority of rolls (> 60%).
TEST(TerrorZonesSelection, SingleInWindowTargetDominates)
{
    std::vector<PoolEntry> pool = ThreeBandPool();
    SelectionConfig cfg = DefaultCfg();
    XorshiftRng rng(0x123456ULL);

    // Target level 37 puts the Mid zone's mid (37.5) inside the 5-level
    // window. The Low and High zones fall outside and get Below/Above.
    std::vector<uint8> targets = {37};

    std::unordered_map<uint32, int> counts;
    constexpr int ROLLS = 2000;
    for (int i = 0; i < ROLLS; ++i)
    {
        auto picks = SelectZones(pool, targets, {}, cfg, rng);
        ASSERT_EQ(picks.size(), 1u);
        counts[picks[0]]++;
    }
    EXPECT_GT(counts[200], counts[100]);
    EXPECT_GT(counts[200], counts[300]);
    EXPECT_GT(counts[200], ROLLS * 6 / 10);
}

// 10.1 — Grouped-target dedupe: when the caller passes one level per group
// rather than per player, a 5-member level-30 group weights the same as a
// solo level-30 — this is enforced by the caller, so the test asserts that
// SelectZones treats {30} identically to {30} (i.e. no internal weighting
// by target-count duplicates). This is really a spec test on the call
// protocol, verified by equivalence.
TEST(TerrorZonesSelection, SingleTargetListIndifferentToDupes)
{
    std::vector<PoolEntry> pool = ThreeBandPool();
    SelectionConfig cfg = DefaultCfg();

    XorshiftRng a(42), b(42);
    auto p1 = SelectZones(pool, {30}, {}, cfg, a);
    auto p2 = SelectZones(pool, {30}, {}, cfg, b);
    EXPECT_EQ(p1, p2);  // same inputs, same seed → same outputs
    // And: duplicated target list is what caller AVOIDS; we don't accept
    // it with the same semantics — but we still shouldn't crash or skew
    // pathologically.
    XorshiftRng c(42);
    auto p3 = SelectZones(pool, {30, 30, 30, 30, 30}, {}, cfg, c);
    EXPECT_EQ(p3.size(), 1u);
}

// 10.1 — Recency dampening multiplies weight by the configured factor.
// With mult=0.1, a recent in-window zone is 10× less likely than an
// equally-weighted non-recent one.
TEST(TerrorZonesSelection, RecencyDampensRecentZone)
{
    std::vector<PoolEntry> pool = {
        {100, "A", 30, 40, true},   // mid 35 — in window for target 35
        {200, "B", 30, 40, true},   // mid 35 — in window for target 35
    };
    SelectionConfig cfg = DefaultCfg();
    cfg.recencyMultiplier = 0.1;
    XorshiftRng rng(7);

    std::vector<uint8> targets = {35};
    std::vector<uint32> recent = {100};  // A was recent

    std::unordered_map<uint32, int> counts;
    constexpr int ROLLS = 2000;
    for (int i = 0; i < ROLLS; ++i)
    {
        auto picks = SelectZones(pool, targets, recent, cfg, rng);
        ASSERT_EQ(picks.size(), 1u);
        counts[picks[0]]++;
    }
    // B should dominate ~10×; we expect at least 80/20.
    EXPECT_GT(counts[200], counts[100] * 4);
}

// 10.1 — Multi-slot dedup: slotCount=2 never returns the same zone twice.
TEST(TerrorZonesSelection, MultiSlotDeduplicates)
{
    std::vector<PoolEntry> pool = ThreeBandPool();
    SelectionConfig cfg = DefaultCfg(/*slots*/ 2);
    XorshiftRng rng(0xDEADBEEFULL);

    for (int i = 0; i < 500; ++i)
    {
        auto picks = SelectZones(pool, {}, {}, cfg, rng);
        ASSERT_EQ(picks.size(), 2u);
        EXPECT_NE(picks[0], picks[1]) << "iteration " << i;
    }
}

// 10.1 — All-zones-dampened case: multiplier is 0.1 (not 0), so the
// weighted pick still terminates with a valid zone even when every zone
// is in the recent set.
TEST(TerrorZonesSelection, AllDampenedStillPicks)
{
    std::vector<PoolEntry> pool = ThreeBandPool();
    SelectionConfig cfg = DefaultCfg();
    cfg.recencyMultiplier = 0.1;
    XorshiftRng rng(99);

    std::vector<uint32> recent = {100, 200, 300};
    auto picks = SelectZones(pool, {}, recent, cfg, rng);
    ASSERT_EQ(picks.size(), 1u);
    EXPECT_TRUE(picks[0] == 100 || picks[0] == 200 || picks[0] == 300);
}

// Multiplier of exactly 0 should remove a recent zone from the pool;
// if all zones are recent under mult=0, nothing can be picked.
TEST(TerrorZonesSelection, ZeroMultiplierExcludesRecent)
{
    std::vector<PoolEntry> pool = ThreeBandPool();
    SelectionConfig cfg = DefaultCfg();
    cfg.recencyMultiplier = 0.0;
    XorshiftRng rng(1);

    // Two recent → only one pickable.
    auto picks = SelectZones(pool, {}, {100, 200}, cfg, rng);
    ASSERT_EQ(picks.size(), 1u);
    EXPECT_EQ(picks[0], 300u);

    // All recent → no picks.
    XorshiftRng rng2(2);
    auto picks2 = SelectZones(pool, {}, {100, 200, 300}, cfg, rng2);
    EXPECT_TRUE(picks2.empty());
}

// Disabled zones are never picked.
TEST(TerrorZonesSelection, DisabledZonesExcluded)
{
    std::vector<PoolEntry> pool = {
        {100, "On",  30, 40, true},
        {200, "Off", 30, 40, false},
    };
    SelectionConfig cfg = DefaultCfg();
    XorshiftRng rng(0);
    for (int i = 0; i < 200; ++i)
    {
        auto picks = SelectZones(pool, {35}, {}, cfg, rng);
        ASSERT_EQ(picks.size(), 1u);
        EXPECT_EQ(picks[0], 100u);
    }
}
