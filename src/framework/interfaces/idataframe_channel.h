#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IDATAFRAME_CHANNEL_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IDATAFRAME_CHANNEL_H_

#include "ichannel.h"
#include "idataframe.h"

namespace flowsql {

// {0xe2f3a4b5-cdef-0123-4567-89abcdef0123}
const Guid IID_DATAFRAME_CHANNEL = {0xe2f3a4b5, 0xcdef, 0x0123, {0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23}};

// IDataFrameChannel — DataFrame 通道子接口
// Read() 快照语义（非破坏性），Write() 替换语义
interface IDataFrameChannel : public IChannel {
    // 将 DataFrame 写入通道（替换语义，覆盖当前内容）
    virtual int Write(IDataFrame* df) = 0;

    // 从通道读取 DataFrame（快照语义，非破坏性，可多次读取）
    virtual int Read(IDataFrame* df) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IDATAFRAME_CHANNEL_H_
