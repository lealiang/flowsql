#include <common/iplugin.h>
#include <framework/interfaces/ibuiltin_registry.h>

#include "builtin_registry_plugin.h"

BEGIN_PLUGIN_REGIST(flowsql::builtin::BuiltinRegistryPlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
    ____INTERFACE(flowsql::IID_BUILTIN_REGISTRY, flowsql::IBuiltinRegistry)
END_PLUGIN_REGIST()
