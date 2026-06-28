#pragma once
// ProfessionMats.h – comprehensive reagent tables for ModDynamicAH
// Each table is std::array<MatBracket,N> following WotLK trainer breakpoints.

#include <cstdint>
#include <initializer_list>
#include <array>

struct MatBracket
{
    uint16_t minSkill, maxSkill;
    std::initializer_list<uint32_t> items;
};

// -------------------------------- Cloth / Bandage (Tailoring + First Aid)
static const std::array<MatBracket, 7> TAILORING_CLOTH = {{
    {1, 75, {2589 /* Linen Cloth */}},
    {75, 125, {2592 /* Wool Cloth */}},
    {125, 175, {4306 /* Silk Cloth */}},
    {175, 225, {4338 /* Mageweave Cloth */}},
    {225, 300, {14047 /* Runecloth */}},
    {300, 350, {21877 /* Netherweave Cloth */}},
    {350, 450, {33470 /* Frostweave Cloth */}},
}};

// -------------------------------- Herbs (Herbalism / Alchemy / Inscription)
static const std::array<MatBracket, 12> HERBS = {{
    {1, 70, {765 /* Silverleaf */, 2447 /* Peacebloom */}},
    {70, 115, {2449 /* Earthroot */, 785 /* Mageroyal */, 2450 /* Briarthorn */}},
    {115, 165, {2453 /* Bruiseweed */, 3820 /* Stranglekelp */, 2452 /* Swiftthistle */}},
    {165, 205, {3356 /* Kingsblood */, 3369 /* Grave Moss */, 3355 /* Wild Steelbloom */}},
    {205, 230, {3358 /* Khadgar's Whisker */, 3818 /* Fadeleaf */, 3819 /* Wintersbite */}},
    {230, 270, {3821 /* Goldthorn */, 4625 /* Firebloom */, 8836 /* Arthas' Tears */}},
    {270, 300, {8831 /* Purple Lotus */, 8839 /* Blindweed */, 8845 /* Ghost Mushroom */, 13463 /* Dreamfoil */, 13464 /* Golden Sansam */}},
    {300, 330, {22785 /* Felweed */, 22786 /* Dreaming Glory */, 22787 /* Ragveil */, 22789 /* Terocone */}},
    {330, 375, {22790 /* Ancient Lichen */, 22791 /* Netherbloom */, 22792 /* Nightmare Vine */, 22793 /* Mana Thistle */}},
    {375, 400, {36901 /* Goldclover */, 36903 /* Adder's Tongue */, 37921 /* Deadnettle */}},
    {400, 425, {36904 /* Tiger Lily */, 36907 /* Talandra's Rose */, 36905 /* Lichbloom */}},
    {425, 450, {36906 /* Icethorn */, 36908 /* Frost Lotus */}},
}};

// -------------------------------- Mining: Ore (Mining, JC prospecting)
static const std::array<MatBracket, 9> MINING_ORE = {{
    {1, 65, {2770 /* Copper Ore */}},
    {65, 125, {2771 /* Tin Ore */}},
    {125, 175, {2772 /* Iron Ore */, 2775 /* Silver Ore */}},
    {175, 230, {3858 /* Mithril Ore */, 7911 /* Truesilver Ore */}},
    {230, 300, {10620 /* Thorium Ore */}},
    {300, 325, {23424 /* Fel Iron Ore */}},
    {325, 350, {23425 /* Adamantite Ore */}},
    {350, 395, {36909 /* Cobalt Ore */}},
    {395, 450, {36912 /* Saronite Ore */, 36910 /* Titanium Ore */}},
}};

// -------------------------------- Blacksmithing bars (trainable path)
static const std::array<MatBracket, 10> BS_BARS = {{
    {1, 75, {2840 /* Copper Bar */}},
    {75, 125, {2841 /* Bronze Bar */, 3576 /* Tin Bar */}},
    {125, 150, {3575 /* Iron Bar */}},
    {150, 200, {3859 /* Steel Bar */, 3577 /* Gold Bar */}},
    {200, 250, {3860 /* Mithril Bar */}},
    {250, 300, {12359 /* Thorium Bar */}},
    {300, 325, {23445 /* Fel Iron Bar */}},
    {325, 350, {23446 /* Adamantite Bar */}},
    {350, 420, {36916 /* Cobalt Bar */}},
    {420, 450, {36913 /* Saronite Bar */}},
}};

// -------------------------------- Leathers (Skinning / Leatherworking)
static const std::array<MatBracket, 7> LEATHERS = {{
    {1, 75, {2318 /* Light Leather */}},
    {75, 125, {2319 /* Medium Leather */}},
    {125, 200, {4234 /* Heavy Leather */}},
    {200, 250, {4304 /* Thick Leather */}},
    {250, 300, {8170 /* Rugged Leather */}},
    {300, 350, {21887 /* Knothide Leather */}},
    {350, 450, {33568 /* Borean Leather */}},
}};

// -------------------------------- Enchanting dusts
static const std::array<MatBracket, 7> ENCH_DUSTS = {{
    {1, 120, {10940 /* Strange Dust */}},
    {120, 180, {11083 /* Soul Dust */}},
    {180, 240, {11137 /* Vision Dust */}},
    {240, 300, {11176 /* Dream Dust */}},
    {300, 325, {16204 /* Illusion Dust */}},
    {325, 375, {22445 /* Arcane Dust (BC) */}},
    {375, 450, {34054 /* Infinite Dust */}},
}};

// -------------------------------- Stones (Engineering bombs, etc.)
static const std::array<MatBracket, 5> MINING_STONE = {{
    {1, 65, {2835 /* Rough Stone */}},
    {65, 125, {2836 /* Coarse Stone */}},
    {125, 175, {2838 /* Heavy Stone */}},
    {175, 250, {7912 /* Solid Stone */}},
    {250, 450, {12365 /* Dense Stone */}},
}};

// -------------------------------- Cooking meats
static const std::array<MatBracket, 8> COOKING_MEAT = {{
    {1, 60, {769 /* Chunk of Boar Meat */, 2672 /* Stringy Wolf Meat */}},
    {60, 120, {3173 /* Bear Meat */, 3667 /* Tender Crocolisk Meat */}},
    {120, 180, {3730 /* Big Bear Meat */, 3731 /* Lion Meat */}},
    {180, 240, {3712 /* Turtle Meat */, 12223 /* Meaty Bat Wing */}},
    {240, 300, {3174 /* Spider Ichor */, 12037 /* Mystery Meat */}},
    {300, 325, {27671 /* Buzzard Meat */, 27678 /* Clefthoof Meat */}},
    {325, 350, {27682 /* Talbuk Venison */, 31670 /* Raptor Ribs */}},
    {350, 450, {43013 /* Chilled Meat */, 43009 /* Shoveltusk Flank */}},
}};

// -------------------------------- Fishing / raw fish
static const std::array<MatBracket, 8> FISHING_RAW = {{
    {1, 75, {6289 /* Raw Longjaw Mud Snapper */, 6291 /* Raw Brilliant Smallfish */}},
    {75, 150, {6308 /* Raw Bristle Whisker Catfish */, 6362 /* Raw Rockscale Cod */}},
    {150, 225, {6359 /* Firefin Snapper */, 6361 /* Raw Rainbow Fin Albacore */}},
    {225, 300, {13754 /* Raw Glossy Mightfish */, 13758 /* Raw Redgill */}},
    {300, 325, {27422 /* Barbed Gill Trout */, 27425 /* Spotted Feltail */}},
    {325, 350, {27429 /* Zangarian Sporefish */, 27437 /* Icefin Bluefish */}},
    {350, 400, {41809 /* Glacial Salmon */, 41802 /* Imperial Manta Ray */}},
    {400, 450, {41808 /* Bonescale Snapper */, 53063 /* Mountain Trout */}},
}};

// -------------------------------- JC prospect gems
static const std::array<MatBracket, 8> JEWELCRAFT_GEMS = {{
    {1, 180, {774 /* Malachite */, 818 /* Tigerseye */, 1210 /* Shadowgem */, 1206 /* Moss Agate */}},
    {180, 230, {1705 /* Lesser Moonstone */, 1529 /* Jade */}},
    {230, 300, {7910 /* Star Ruby */, 7909 /* Aquamarine */, 3864 /* Citrine */}},
    {300, 325, {23112 /* Golden Draenite */, 23107 /* Shadow Draenite */}},
    {325, 350, {23436 /* Living Ruby */, 23440 /* Dawnstone */}},
    // Northrend common (uncut, Cobalt prospecting)
    {350, 400, {36917 /* Bloodstone */, 36920 /* Sun Crystal */, 36923 /* Chalcedony */, 36926 /* Shadow Crystal */, 36929 /* Huge Citrine */, 36932 /* Dark Jade */}},
    // Northrend rare (uncut, Saronite prospecting)
    {400, 440, {36918 /* Scarlet Ruby */, 36921 /* Autumn's Glow */, 36924 /* Sky Sapphire */, 36927 /* Twilight Opal */, 36930 /* Monarch Topaz */, 36933 /* Forest Emerald */}},
    // Northrend epic (uncut, Titanium prospecting / transmute)
    {440, 450, {36919 /* Cardinal Ruby */, 36922 /* King's Amber */, 36925 /* Majestic Zircon */, 36928 /* Dreadstone */, 36931 /* Ametrine */, 36934 /* Eye of Zul */}},
}};

// -------------------------------- Enchanting essences
static const std::array<MatBracket, 7> ENCH_ESSENCE = {{
    {1, 70, {10938 /* Lesser Magic Essence */}},
    {70, 150, {10998 /* Lesser Astral Essence */}},
    {150, 225, {11134 /* Lesser Mystic Essence */, 11174 /* Lesser Nether Essence */}},
    {225, 300, {16202 /* Lesser Eternal Essence */, 16203 /* Greater Eternal Essence */}},
    {300, 325, {22447 /* Lesser Planar Essence */}},
    {325, 375, {22446 /* Greater Planar Essence */}},
    {375, 450, {34056 /* Lesser Cosmic Essence */, 34055 /* Greater Cosmic Essence */}},
}};

// -------------------------------- Enchanting shards (rods are tools, not listed)
static const std::array<MatBracket, 6> ENCH_SHARDS = {{
    {1, 150, {10978 /* Small Glimmering Shard */, 11084 /* Large Glimmering Shard */}},
    {150, 225, {11138 /* Small Glowing Shard */, 11139 /* Large Glowing Shard */}},
    {225, 285, {14343 /* Small Brilliant Shard */, 14344 /* Large Brilliant Shard */}},
    {285, 350, {22448 /* Small Prismatic Shard */, 22449 /* Large Prismatic Shard */}},
    {350, 425, {34053 /* Small Dream Shard */, 34052 /* Dream Shard */}},
    {425, 450, {34057 /* Abyss Crystal */}},
}};

// -------------------------------- Elementals / Primals / Eternals (Alchemy / crafting)
static const std::array<MatBracket, 3> ELEMENTALS = {{
    {300, 375, {22451 /* Primal Air */, 22452 /* Primal Earth */, 21884 /* Primal Fire */, 21885 /* Primal Water */, 22456 /* Primal Shadow */, 22457 /* Primal Mana */}},
    {375, 425, {37700 /* Crystallized Air */, 37701 /* Crystallized Earth */, 37702 /* Crystallized Fire */, 37703 /* Crystallized Shadow */, 37704 /* Crystallized Life */, 37705 /* Crystallized Water */}},
    {425, 450, {35622 /* Eternal Water */, 35623 /* Eternal Air */, 35624 /* Eternal Earth */, 35625 /* Eternal Life */, 35627 /* Eternal Shadow */, 36860 /* Eternal Fire */}},
}};

// -------------------------------- Rare raws / special mats
static const std::array<MatBracket, 4> RARE_RAW = {{
    {250, 310, {12655 /* Enchanted Thorium Bar */}},
    {330, 375, {23571 /* Primal Might */, 25707 /* Fel Hide */}},
    {375, 450, {33568 /* Borean Leather */, 43007 /* Northern Spices */, 45087 /* Runed Orb */}},
    {430, 450, {47556 /* Crusader Orb */}},
}};

// -------------------------------- Inscription pigments (milled from herbs)
static const std::array<MatBracket, 8> INSCRIPTION_PIGMENT = {{
    {1, 75, {39151 /* Alabaster Pigment */}},
    {75, 125, {39334 /* Dusky Pigment */}},
    {125, 175, {39338 /* Golden Pigment */}},
    {175, 225, {39339 /* Emerald Pigment */, 39340 /* Violet Pigment */}},
    {225, 300, {39341 /* Silvery Pigment */}},
    {300, 375, {39342 /* Nether Pigment */, 39343 /* Azure Pigment */}},
    {375, 425, {43103 /* Verdant Pigment */, 43104 /* Burnt Pigment */}},
    {425, 450, {43105 /* Indigo Pigment */}},
}};

// -------------------------------- Inscription inks (milled/ground from pigments)
static const std::array<MatBracket, 6> INSCRIPTION_INK = {{
    {1, 75, {37101 /* Ivory Ink */, 39469 /* Moonglow Ink */}},
    {75, 175, {39774 /* Midnight Ink */}},
    {175, 300, {43116 /* Lion's Ink */}},
    {300, 375, {43118 /* Jadefire Ink */, 43122 /* Shimmering Ink */}},
    {375, 425, {43124 /* Ethereal Ink */}},
    {425, 450, {43126 /* Ink of the Sea */}},
}};
