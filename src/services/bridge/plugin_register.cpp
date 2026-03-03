#include <common/typedef.h>
#include <common/iplugin.h>

#include "framework/interfaces/ibridge.h"
#include "bridge_plugin.h"

// 注册 BridgePlugin 为动态库插件，同时暴露 IPlugin 和 IBridge 接口
BEGIN_PLUGIN_REGIST(flowsql::bridge::BridgePlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
    ____INTERFACE(flowsql::IID_BRIDGE, flowsql::IBridge)
END_PLUGIN_REGIST()
