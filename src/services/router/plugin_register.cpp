/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <common/iplugin.h>

#include "router_agency_plugin.h"

// 注册 RouterAgencyPlugin 为动态库插件
BEGIN_PLUGIN_REGIST(flowsql::router::RouterAgencyPlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
END_PLUGIN_REGIST()
