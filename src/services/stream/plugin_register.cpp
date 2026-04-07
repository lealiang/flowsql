/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <common/iplugin.h>
#include <framework/interfaces/istream_factory.h>
#include <framework/interfaces/istream_manager.h>

#include "stream_plugin.h"

BEGIN_PLUGIN_REGIST(flowsql::stream::StreamPlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
    ____INTERFACE(flowsql::IID_STREAM_FACTORY, flowsql::IStreamFactory)
    ____INTERFACE(flowsql::IID_STREAM_MANAGER, flowsql::IStreamManager)
END_PLUGIN_REGIST()
