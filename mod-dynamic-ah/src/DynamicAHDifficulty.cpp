#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "Field.h"
#include "DynamicAHDifficulty.h"
#include "Log.h"

namespace ModDynamicAH
{

    static std::unordered_map<uint32, uint16> g_skillByItem;
    static std::unordered_map<uint32, uint8> g_minCreatureLvlByItem;

    std::unordered_map<uint32, uint16> &DynamicAHDifficulty::SkillMap() { return g_skillByItem; }
    std::unordered_map<uint32, uint8> &DynamicAHDifficulty::CreatureLvlMap() { return g_minCreatureLvlByItem; }

    static inline void EmplaceMax(std::unordered_map<uint32, uint16> &m, uint32 id, uint16 val)
    {
        auto it = m.find(id);
        if (it == m.end())
        {
            m.emplace(id, val);
            return;
        }
        if (val > it->second)
            it->second = val;
    }

    void DynamicAHDifficulty::Build()
    {
        g_skillByItem.clear();
        g_minCreatureLvlByItem.clear();

        // A) From DBC: SkillLineAbility -> Spell -> Reagents
        for (SkillLineAbilityEntry const *abl : sSkillLineAbilityStore)
        {
            if (!abl || abl->MinSkillLineRank == 0)
                continue;
            SpellInfo const *si = sSpellMgr->GetSpellInfo(abl->Spell);
            if (!si)
                continue;

            for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                if (uint32 item = si->Reagent[i])
                    EmplaceMax(g_skillByItem, item, abl->MinSkillLineRank);
        }

        // B) DB: min creature level that drops the item
        if (QueryResult qr = WorldDatabase.Query(R"SQL(
        SELECT l.item, MIN(c.minlevel)
        FROM creature_loot_template l
        JOIN creature_template c ON c.entry = l.entry
        WHERE l.item > 0
        GROUP BY l.item
    )SQL"))
        {
            do
            {
                Field *f = qr->Fetch();
                uint32 item = f[0].Get<uint32>();
                uint8 lvl = uint8(std::min<uint32>(f[1].Get<uint32>(), 80u));
                g_minCreatureLvlByItem[item] = lvl;
            } while (qr->NextRow());
        }

        LOG_INFO("mod.dynamicah", "DynamicAHDifficulty: built skill={} creatureLvls={}",
                 g_skillByItem.size(), g_minCreatureLvlByItem.size());
    }

    uint16 DynamicAHDifficulty::MaxReqSkillForItem(uint32 itemId)
    {
        auto it = g_skillByItem.find(itemId);
        return it != g_skillByItem.end() ? it->second : 0;
    }

    uint8 DynamicAHDifficulty::MinCreatureLevelDropping(uint32 itemId)
    {
        auto it = g_minCreatureLvlByItem.find(itemId);
        return it != g_minCreatureLvlByItem.end() ? it->second : 0;
    }

} // namespace ModDynamicAH
