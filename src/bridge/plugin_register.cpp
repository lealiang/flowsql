#include <common/typedef.h>
#include <common/loader.hpp>

#include "bridge_plugin.h"

EXPORT_API void pluginunregist() {}

EXPORT_API flowsql::IPlugin* pluginregist(flowsql::IRegister* registry, const char* opt) {
    static flowsql::bridge::BridgePlugin _plugin;

    // 注册 IPlugin（生命周期管理）
    {
        flowsql::IPlugin* iface = dynamic_cast<flowsql::IPlugin*>(&_plugin);
        registry->Regist(flowsql::IID_PLUGIN, iface);
    }
    // 注册 IModule（启停控制）
    {
        flowsql::IModule* iface = dynamic_cast<flowsql::IModule*>(&_plugin);
        registry->Regist(flowsql::IID_MODULE, iface);
    }

    _plugin.Option(opt);
    return &_plugin;
}
