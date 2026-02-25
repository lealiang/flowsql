#ifndef _FLOWSQL_FRAMEWORK_MACROS_H_
#define _FLOWSQL_FRAMEWORK_MACROS_H_

#include <common/loader.hpp>

#include "interfaces/ichannel.h"
#include "interfaces/ioperator.h"

// Channel 注册宏
#define BEGIN_CHANNEL_REGIST(classname)                  \
    BEGIN_PLUGIN_REGIST(classname)                       \
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin) \
    ____INTERFACE(flowsql::IID_CHANNEL, flowsql::IChannel)

#define END_CHANNEL_REGIST() END_PLUGIN_REGIST()

// Operator 注册宏
#define BEGIN_OPERATOR_REGIST(classname)                  \
    BEGIN_PLUGIN_REGIST(classname)                        \
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)  \
    ____INTERFACE(flowsql::IID_OPERATOR, flowsql::IOperator)

#define END_OPERATOR_REGIST() END_PLUGIN_REGIST()

#endif  // _FLOWSQL_FRAMEWORK_MACROS_H_
