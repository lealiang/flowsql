#include <common/typedef.h>
#include <common/loader.hpp>

#include "memory_channel.h"
#include "passthrough_operator.h"

EXPORT_API void pluginunregist() {}

EXPORT_API flowsql::IPlugin* pluginregist(flowsql::IRegister* registry, const char* opt) {
    // MemoryChannel
    static flowsql::MemoryChannel _channel;
    {
        flowsql::IPlugin* iface = dynamic_cast<flowsql::IPlugin*>(&_channel);
        registry->Regist(flowsql::IID_PLUGIN, iface);
    }
    {
        flowsql::IChannel* iface = dynamic_cast<flowsql::IChannel*>(&_channel);
        registry->Regist(flowsql::IID_CHANNEL, iface);
    }
    {
        flowsql::IDataFrameChannel* iface = dynamic_cast<flowsql::IDataFrameChannel*>(&_channel);
        registry->Regist(flowsql::IID_DATAFRAME_CHANNEL, iface);
    }

    // PassthroughOperator
    static flowsql::PassthroughOperator _operator;
    {
        flowsql::IPlugin* iface = dynamic_cast<flowsql::IPlugin*>(&_operator);
        registry->Regist(flowsql::IID_PLUGIN, iface);
    }
    {
        flowsql::IOperator* iface = dynamic_cast<flowsql::IOperator*>(&_operator);
        registry->Regist(flowsql::IID_OPERATOR, iface);
    }

    _channel.Option(opt);
    _operator.Option(opt);
    return &_channel;
}
