#include "DynamicAHRecipes.h"

#include "SpellMgr.h"
#include "SpellInfo.h"
#include "DBCStores.h"
#include "SharedDefines.h"
#include "ObjectMgr.h"
#include "Log.h"

using namespace ModDynamicAH;

RecipeUsageIndex &RecipeUsageIndex::Instance()
{
    static RecipeUsageIndex s_inst;
    return s_inst;
}

void RecipeUsageIndex::EnsureBuilt()
{
    if (!_built)
        Build();
}

uint16_t RecipeUsageIndex::MaxSkillForReagent(uint32_t itemId) const
{
    auto it = _stats.find(itemId);
    if (it == _stats.end() || it->second.count == 0)
        return 0;
    return it->second.max;
}

uint16_t RecipeUsageIndex::EffectiveSkillForReagent(uint32_t itemId) const
{
    auto it = _stats.find(itemId);
    if (it == _stats.end() || it->second.count == 0)
        return 0;

    SkillStats const &st = it->second;

    // Approx median from bins
    uint32_t half = st.count / 2 + (st.count % 2);
    uint32_t cum = 0;
    int medBin = 0;
    for (; medBin < 6; ++medBin)
    {
        cum += st.bins[medBin];
        if (cum >= half)
            break;
    }
    static uint16_t const binMid[6] = {37, 112, 187, 262, 337, 413};
    uint16_t medianEst = binMid[medBin < 0 ? 0 : (medBin > 5 ? 5 : medBin)];

    // Share of high-tier usage (>=300)
    double highShare = st.count ? double(st.bins[4] + st.bins[5]) / double(st.count) : 0.0;

    // Blend median toward max when many recipes are high tier
    double alpha = 0.2 + 0.4 * highShare; // clamp implicitly by formula range
    if (alpha < 0.2)
        alpha = 0.2;
    if (alpha > 0.6)
        alpha = 0.6;

    double eff = (1.0 - alpha) * double(medianEst) + alpha * double(st.max);
    if (eff < 0.0)
        eff = 0.0;
    if (eff > 65535.0)
        eff = 65535.0;
    return uint16_t(eff + 0.5);
}

void RecipeUsageIndex::Build()
{
    if (_built)
        return;

    uint32_t spellsTotal = sSpellStore.GetNumRows();
    uint32_t spellsWithInfo = 0;
    uint64_t reagentSlotsScanned = 0;
    uint64_t reagentSlotsWithItem = 0;
    uint64_t linksChecked = 0;
    uint64_t profLinks = 0;

    for (uint32_t i = 0; i < sSpellStore.GetNumRows(); ++i)
    {
        if (SpellEntry const *se = sSpellStore.LookupEntry(i))
        {
            if (SpellInfo const *info = sSpellMgr->GetSpellInfo(se->Id))
            {
                ++spellsWithInfo;
                for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
                {
                    ++reagentSlotsScanned;
                    if (info->Reagent[r] <= 0)
                        continue;

                    ++reagentSlotsWithItem;
                    uint32_t itemId = uint32_t(info->Reagent[r]);

                    SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(info->Id);
                    for (auto it = bounds.first; it != bounds.second; ++it)
                    {
                        ++linksChecked;

                        SkillLineAbilityEntry const *sla = it->second;
                        if (!sla)
                            continue;

                        SkillLineEntry const *line = sSkillLineStore.LookupEntry(sla->SkillLine);
                        if (!line)
                            continue;

                        if (line->categoryId != SKILL_CATEGORY_PROFESSION &&
                            line->categoryId != SKILL_CATEGORY_SECONDARY)
                            continue;

                        ++profLinks;
                        uint16_t req = std::max<uint16_t>(sla->MinSkillLineRank, sla->TrivialSkillLineRankHigh);
                        SkillStats &st = _stats[itemId];
                        st.count++;
                        if (req < st.min)
                            st.min = req;
                        if (req > st.max)
                            st.max = req;
                        int bin = (req < 75) ? 0 : (req < 150) ? 1
                                               : (req < 225)   ? 2
                                               : (req < 300)   ? 3
                                               : (req < 375)   ? 4
                                                               : 5;
                        st.bins[bin]++;
                    }
                }
            }
        }
    }

    _built = true;

    LOG_INFO("mod.dynamicah",
             "recipes: built reagent index: spellsTotal={} spellsWithInfo={} reagentSlotsScanned={} reagentSlotsWithItem={} linksChecked={} profLinks={} uniqueReagents={}",
             spellsTotal, spellsWithInfo, reagentSlotsScanned, reagentSlotsWithItem, linksChecked, profLinks, _stats.size());
}
