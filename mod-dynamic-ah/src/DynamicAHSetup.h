#pragma once

#include "Chat.h"
#include "DynamicAHState.h"

namespace ModDynamicAH
{
    namespace DynamicAHSetup
    {

        void RunSetup(ModuleState &s, ChatHandler *handler);

        // Fill any unset (0) owner GUID by looking the configured seller character up by name.
        // Used on config load so seller owners survive a server restart without re-running setup.
        void ResolveOwnersByName(ModuleState &s);

    } // namespace DynamicAHSetup
} // namespace ModDynamicAH
