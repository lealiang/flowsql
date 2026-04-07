/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <common/iplugin.h>

#include "gateway_plugin.h"

// 注册 GatewayPlugin 为动态库插件
BEGIN_PLUGIN_REGIST(flowsql::gateway::GatewayPlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
END_PLUGIN_REGIST()
