#include "DynamicAHWorld.h"
#include "DynamicAHCommands.h"

void AddDynamicAhScripts()
{
    new ModDynamicAH::DynamicAHWorld();
    new DynamicAHCommands();
}

void Addmod_dynamic_ahScripts() { AddDynamicAhScripts(); }
