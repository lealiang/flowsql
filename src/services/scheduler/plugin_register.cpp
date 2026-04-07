/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <common/iplugin.h>
#include <framework/interfaces/irouter_handle.h>
#include <framework/interfaces/ischeduler_control_service.h>

#include "scheduler_plugin.h"

// 注册 SchedulerPlugin 为动态库插件，同时注册 IRouterHandle
BEGIN_PLUGIN_REGIST(flowsql::scheduler::SchedulerPlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
    ____INTERFACE(flowsql::IID_ROUTER_HANDLE, flowsql::IRouterHandle)
    ____INTERFACE(flowsql::IID_SCHEDULER_CONTROL_SERVICE, flowsql::ISchedulerControlService)
END_PLUGIN_REGIST()
