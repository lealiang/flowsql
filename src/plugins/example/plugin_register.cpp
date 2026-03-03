#include <common/typedef.h>
#include <common/iplugin.h>

#include "memory_channel.h"
#include "passthrough_operator.h"

EXPORT_API void pluginunregist() {}

EXPORT_API flowsql::IPlugin* pluginregist(flowsql::IRegister* registry, const char* opt) {
    // MemoryChannel — 同时注册为 IPlugin、IChannel、IDataFrameChannel
    static flowsql::MemoryChannel _channel;
    registry->Regist(flowsql::IID_PLUGIN, static_cast<flowsql::IPlugin*>(&_channel));
    registry->Regist(flowsql::IID_CHANNEL, static_cast<flowsql::IChannel*>(&_channel));
    registry->Regist(flowsql::IID_DATAFRAME_CHANNEL, static_cast<flowsql::IDataFrameChannel*>(&_channel));

    // PassthroughOperator — 同时注册为 IPlugin、IOperator
    static flowsql::PassthroughOperator _operator;
    registry->Regist(flowsql::IID_PLUGIN, static_cast<flowsql::IPlugin*>(&_operator));
    registry->Regist(flowsql::IID_OPERATOR, static_cast<flowsql::IOperator*>(&_operator));

    _channel.Option(opt);
    _operator.Option(opt);
    return &_channel;
}
