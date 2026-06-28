#include "Log.h"
#include "ScriptMgr.h"

void AddCustomItemsScripts();

void Addmod_custom_itemsScripts()
{
    LOG_INFO("module", "mod-custom-items: registering scripts.");
    AddCustomItemsScripts();
}
