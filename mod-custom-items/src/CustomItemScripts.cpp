// Script registrations for the custom-item pipeline.
//
//   CustomItems_WorldScript : WorldScript
//     hook OnAfterLoadItemTemplates → mod_custom_items::LoadCustomItems
//
//   CustomItems_AllItemScript : AllItemScript
//     hook OnItemBuildValuesUpdate → rewrite OBJECT_FIELD_ENTRY to the
//     donor entry so the WotLK 3.3.5a client renders the donor's bag
//     icon / 3D model from its unmodified Item.dbc / ItemDisplayInfo.dbc.
//     hook OnItemQueryTemplate → substitute the response template back
//     to the real custom entry's ItemTemplate when the querier owns the
//     custom item, so the tooltip shows the custom name / quality /
//     description. Donor lookups by other players (chat-link, AH
//     browse, mail preview) fall through to the donor's real template.

#include "CustomItems.h"

#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"

namespace
{
    class CustomItems_WorldScript : public WorldScript
    {
    public:
        CustomItems_WorldScript()
            : WorldScript("CustomItems_WorldScript",
                          { WORLDHOOK_ON_AFTER_LOAD_ITEM_TEMPLATES }) {}

        void OnAfterLoadItemTemplates() override
        {
            mod_custom_items::LoadCustomItems();
        }
    };

    class CustomItems_AllItemScript : public AllItemScript
    {
    public:
        CustomItems_AllItemScript()
            : AllItemScript("CustomItems_AllItemScript") {}

        void OnItemBuildValuesUpdate(
            Item const* /*item*/, uint32& entry) override
        {
            entry = mod_custom_items::RewriteWireEntry(entry);
        }

        // Tooltip-query substitution. When a player queries the donor
        // entry (because we rewrote the on-wire entry to that donor
        // earlier), check whether the querier actually owns one of our
        // custom items mapping to that donor. If yes, swap the response
        // template to the custom item's so the player sees their custom
        // name / quality / description in the tooltip. The wire ItemId
        // is enforced by the core handler to equal the queried entry.
        void OnItemQueryTemplate(
            Player const* querier,
            uint32 wireEntry,
            ItemTemplate const*& proto) override
        {
            uint32 customEntry = mod_custom_items::PickCustomEntryForQuery(
                querier, wireEntry);
            if (!customEntry)
                return;
            if (ItemTemplate const* substituted =
                    sObjectMgr->GetItemTemplate(customEntry))
                proto = substituted;
        }
    };
}

void AddCustomItemsScripts()
{
    new CustomItems_WorldScript();
    new CustomItems_AllItemScript();
}
