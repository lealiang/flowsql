/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "task_plugin.h"

BEGIN_PLUGIN_REGIST(flowsql::task::TaskPlugin)
____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
____INTERFACE(flowsql::IID_ROUTER_HANDLE, flowsql::IRouterHandle)
____INTERFACE(flowsql::IID_TASK_STORE, flowsql::ITaskStore)
END_PLUGIN_REGIST()
