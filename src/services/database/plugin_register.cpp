#include <common/iplugin.h>
#include <common/typedef.h>
#include <framework/interfaces/idatabase_factory.h>

#include "database_plugin.h"

// DatabasePlugin — 同时注册为 IPlugin 和 IDatabaseFactory
BEGIN_PLUGIN_REGIST(flowsql::database::DatabasePlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
    ____INTERFACE(flowsql::IID_DATABASE_FACTORY, flowsql::IDatabaseFactory)
END_PLUGIN_REGIST()
