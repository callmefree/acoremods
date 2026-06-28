#pragma once

#include <cstdint>
#include <unordered_map>
#include <array>

namespace ModDynamicAH
{

struct SkillStats
{
    uint16_t min = 65535;
    uint16_t max = 0;
    uint32_t count = 0;
    std::array<uint32_t, 6> bins{ {0,0,0,0,0,0} }; // [0-75), [75-150), [150-225), [225-300), [300-375), [375-450+]
};

// Encapsulated index of reagent -> recipe skill distribution.
class RecipeUsageIndex
{
public:
    static RecipeUsageIndex& Instance();

    // Ensure the index is built (idempotent).
    void EnsureBuilt();

    // Highest profession difficulty among recipes using this item as reagent.
    uint16_t MaxSkillForReagent(uint32_t itemId) const;

    // Effective difficulty (median blended toward max when usage skews late-game).
    uint16_t EffectiveSkillForReagent(uint32_t itemId) const;

private:
    void Build(); // one-time
    bool _built = false;
    std::unordered_map<uint32_t, SkillStats> _stats;
};

} // namespace ModDynamicAH
