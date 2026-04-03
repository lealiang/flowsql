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

// {0xa7b8c9d0-e1f2-3456-789a-bcdef0123456}
const Guid IID_APPENDABLE_DATAFRAME_CHANNEL = {0xa7b8c9d0, 0xe1f2, 0x3456, {0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56}};

// IAppendableDataFrameChannel — 可选扩展接口
// 在保持 Write() 替换语义不变的前提下，提供追加写能力
interface IAppendableDataFrameChannel : public IDataFrameChannel {
    // 追加写：将 df 的数据追加到当前通道尾部
    // 要求 schema 与当前通道一致；若通道为空则以 df schema 初始化
    virtual int Append(IDataFrame* df) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IDATAFRAME_CHANNEL_H_
