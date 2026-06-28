// Pure-function unit tests for Slice 9 Pass 1 class-drop helpers.
// Covers the entry-encode round trip across all 1,920 cells, the
// class+spec → archetype mapping for every WotLK class, and the
// out-of-range guards. Integration with the loot pipeline is
// exercised separately via in-game GM walk.

#include "TerrorZonesMgr.h"

#include <cstdint>
#include <set>

#include <gtest/gtest.h>

using namespace mod_terror_zones;

// ---------------------------------------------------------------------
// Encode / decode round-trip
// ---------------------------------------------------------------------

TEST(TerrorZonesItemContent, EncodeMatchesPassZeroOfContentCell)
{
    // T5 band-80 STR-DPS helm (head). Under the 5-tier / 5-playstyle
    // / 12-slot encoding: 700100 + 7*300 + 4*60 + 0*12 + 0 = 702440.
    uint32 entry = EncodeClassDropEntry(7, TIER_5,
                                         ARCHETYPE_STR_DPS,
                                         ARMOR_SLOT_HEAD);
    EXPECT_EQ(entry, 702440u);
}

TEST(TerrorZonesItemContent, EncodeDecodeRoundTripAllCells)
{
    std::set<uint32> seen;
    for (uint8 band = 0; band < 8; ++band)
    {
        for (uint8 tIdx = 0; tIdx < 5; ++tIdx)
        {
            Tier tier = static_cast<Tier>(tIdx + 1);  // T1..T5
            for (uint8 aIdx = 0; aIdx < 5; ++aIdx)
            {
                Archetype arch = static_cast<Archetype>(aIdx + 1);
                for (uint8 sIdx = 0; sIdx < ARMOR_SLOT_COUNT; ++sIdx)
                {
                    ArmorSlot slot = static_cast<ArmorSlot>(sIdx);
                    uint32 entry = EncodeClassDropEntry(band, tier,
                                                        arch, slot);
                    ASSERT_NE(entry, 0u)
                        << "encode failed at band=" << +band
                        << " tier=" << +tier << " arch=" << +arch
                        << " slot=" << +slot;
                    ASSERT_TRUE(seen.insert(entry).second)
                        << "duplicate entry " << entry;
                    uint8 dBand = 0;
                    Tier dTier = TIER_NONE;
                    Archetype dArch = ARCHETYPE_NONE;
                    ArmorSlot dSlot = ARMOR_SLOT_HEAD;
                    ASSERT_TRUE(DecodeClassDropEntry(
                        entry, dBand, dTier, dArch, dSlot));
                    EXPECT_EQ(dBand, band);
                    EXPECT_EQ(dTier, tier);
                    EXPECT_EQ(dArch, arch);
                    EXPECT_EQ(dSlot, slot);
                }
            }
        }
    }
    // 8 bands × 5 tiers × 5 archetypes × 13 slots = 2600 cells
    // (12 armor in [700100, 702500) + 1 weapon in [703000, 703200)).
    EXPECT_EQ(seen.size(), 2600u);
    EXPECT_EQ(*seen.begin(), 700100u);
    EXPECT_EQ(*seen.rbegin(), 703199u);
}

TEST(TerrorZonesItemContent, EncodeRejectsOutOfRangeInput)
{
    EXPECT_EQ(EncodeClassDropEntry(8, TIER_2, ARCHETYPE_TANK,
                                    ARMOR_SLOT_CHEST), 0u);
    // T1 is now valid; only TIER_NONE and out-of-range tier reject.
    EXPECT_EQ(EncodeClassDropEntry(0, TIER_NONE, ARCHETYPE_TANK,
                                    ARMOR_SLOT_CHEST), 0u);
    EXPECT_EQ(EncodeClassDropEntry(0, TIER_2, ARCHETYPE_NONE,
                                    ARMOR_SLOT_CHEST), 0u);
    EXPECT_EQ(EncodeClassDropEntry(0, TIER_2, ARCHETYPE_TANK,
                                    ARMOR_SLOT_COUNT), 0u);
}

TEST(TerrorZonesItemContent, DecodeRejectsOutOfRangeEntry)
{
    uint8 band = 99;
    Tier tier = TIER_NONE;
    Archetype arch = ARCHETYPE_NONE;
    ArmorSlot slot = ARMOR_SLOT_HEAD;

    EXPECT_FALSE(DecodeClassDropEntry(0, band, tier, arch, slot));
    EXPECT_FALSE(DecodeClassDropEntry(700099u, band, tier, arch, slot));
    EXPECT_FALSE(DecodeClassDropEntry(702500u, band, tier, arch, slot));
    EXPECT_FALSE(DecodeClassDropEntry(0xFFFFFFFFu,
                                       band, tier, arch, slot));

    // Out params unchanged on failure.
    EXPECT_EQ(band, 99);
    EXPECT_EQ(tier, TIER_NONE);
    EXPECT_EQ(arch, ARCHETYPE_NONE);
    EXPECT_EQ(slot, ARMOR_SLOT_HEAD);
}

// ---------------------------------------------------------------------
// Archetype mapping (5 playstyles)
// ---------------------------------------------------------------------

TEST(TerrorZonesItemContent, ArchetypeWarriorByTree)
{
    EXPECT_EQ(ArchetypeForClassSpec(1, 0), ARCHETYPE_STR_DPS);  // Arms
    EXPECT_EQ(ArchetypeForClassSpec(1, 1), ARCHETYPE_STR_DPS);  // Fury
    EXPECT_EQ(ArchetypeForClassSpec(1, 2), ARCHETYPE_TANK);     // Prot
}

TEST(TerrorZonesItemContent, ArchetypePaladinByTree)
{
    EXPECT_EQ(ArchetypeForClassSpec(2, 0), ARCHETYPE_HEALER);   // Holy
    EXPECT_EQ(ArchetypeForClassSpec(2, 1), ARCHETYPE_TANK);     // Prot
    EXPECT_EQ(ArchetypeForClassSpec(2, 2), ARCHETYPE_STR_DPS);  // Ret
}

TEST(TerrorZonesItemContent, ArchetypeHunterRogueAllSpecs)
{
    for (uint8 s = 0; s < 3; ++s)
    {
        EXPECT_EQ(ArchetypeForClassSpec(3, s), ARCHETYPE_AGI_DPS);
        EXPECT_EQ(ArchetypeForClassSpec(4, s), ARCHETYPE_AGI_DPS);
    }
}

TEST(TerrorZonesItemContent, ArchetypePriestByTree)
{
    EXPECT_EQ(ArchetypeForClassSpec(5, 0), ARCHETYPE_HEALER);   // Disc
    EXPECT_EQ(ArchetypeForClassSpec(5, 1), ARCHETYPE_HEALER);   // Holy
    EXPECT_EQ(ArchetypeForClassSpec(5, 2), ARCHETYPE_CASTER);   // Shadow
}

TEST(TerrorZonesItemContent, ArchetypeDeathKnightByTree)
{
    EXPECT_EQ(ArchetypeForClassSpec(6, 0), ARCHETYPE_TANK);     // Blood
    EXPECT_EQ(ArchetypeForClassSpec(6, 1), ARCHETYPE_STR_DPS);  // Frost
    EXPECT_EQ(ArchetypeForClassSpec(6, 2), ARCHETYPE_STR_DPS);  // Unholy
}

TEST(TerrorZonesItemContent, ArchetypeShamanByTree)
{
    EXPECT_EQ(ArchetypeForClassSpec(7, 0), ARCHETYPE_CASTER);   // Ele
    EXPECT_EQ(ArchetypeForClassSpec(7, 1), ARCHETYPE_AGI_DPS);  // Enh
    EXPECT_EQ(ArchetypeForClassSpec(7, 2), ARCHETYPE_HEALER);   // Resto
}

TEST(TerrorZonesItemContent, ArchetypeMageWarlockAllSpecs)
{
    for (uint8 s = 0; s < 3; ++s)
    {
        EXPECT_EQ(ArchetypeForClassSpec(8, s), ARCHETYPE_CASTER);
        EXPECT_EQ(ArchetypeForClassSpec(9, s), ARCHETYPE_CASTER);
    }
}

TEST(TerrorZonesItemContent, ArchetypeDruidByTree)
{
    EXPECT_EQ(ArchetypeForClassSpec(11, 0), ARCHETYPE_CASTER);   // Bal
    EXPECT_EQ(ArchetypeForClassSpec(11, 1), ARCHETYPE_AGI_DPS);  // Feral
    EXPECT_EQ(ArchetypeForClassSpec(11, 2), ARCHETYPE_HEALER);   // Resto
}

TEST(TerrorZonesItemContent, ArchetypeUnknownClassReturnsNone)
{
    EXPECT_EQ(ArchetypeForClassSpec(0,  0), ARCHETYPE_NONE);
    EXPECT_EQ(ArchetypeForClassSpec(10, 0), ARCHETYPE_NONE);  // gap
    EXPECT_EQ(ArchetypeForClassSpec(12, 0), ARCHETYPE_NONE);
    EXPECT_EQ(ArchetypeForClassSpec(255, 0), ARCHETYPE_NONE);
}

TEST(TerrorZonesItemContent, ArchetypeUnknownSpecReturnsNone)
{
    EXPECT_EQ(ArchetypeForClassSpec(1, 3), ARCHETYPE_NONE);
    EXPECT_EQ(ArchetypeForClassSpec(1, 99), ARCHETYPE_NONE);
}
