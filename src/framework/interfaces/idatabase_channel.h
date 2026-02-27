#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_CHANNEL_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_CHANNEL_H_

#include <cstddef>
#include <cstdint>

#include "ichannel.h"

namespace flowsql {

// {0xf3a4b5c6-def0-1234-5678-9abcdef01234}
const Guid IID_DATABASE_CHANNEL = {0xf3a4b5c6, 0xdef0, 0x1234, {0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34}};

// 写入统计信息
struct BatchWriteStats {
    int64_t rows_written = 0;
    int64_t bytes_written = 0;
    int64_t elapsed_ms = 0;
};

// IBatchReader — 流式读取器
// 生命周期：CreateReader() → GetSchema() → Next()... → Close() → Release()
interface IBatchReader {
    virtual ~IBatchReader() = default;

    // 获取结果集 Schema（Arrow Schema IPC 序列化）
    virtual int GetSchema(const uint8_t** buf, size_t* len) = 0;

    // 读取下一批数据（Arrow RecordBatch IPC buffer）
    // 返回：0=有数据, 1=已读完, <0=错误
    virtual int Next(const uint8_t** buf, size_t* len) = 0;

    // 取消正在进行的读取
    virtual void Cancel() = 0;

    // 关闭读取器
    virtual void Close() = 0;

    // 错误信息
    virtual const char* GetLastError() = 0;

    // 释放读取器自身
    virtual void Release() = 0;
};

// IBatchWriter — 批量写入器
// 生命周期：CreateWriter() → Write()... → Flush() → Close() → Release()
interface IBatchWriter {
    virtual ~IBatchWriter() = default;

    // 写入一批数据（Arrow RecordBatch IPC buffer）
    virtual int Write(const uint8_t* buf, size_t len) = 0;

    // 强制刷新缓冲区到数据库
    virtual int Flush() = 0;

    // 关闭写入器，返回统计信息（stats 可为 nullptr）
    virtual void Close(BatchWriteStats* stats) = 0;

    // 错误信息
    virtual const char* GetLastError() = 0;

    // 释放写入器自身
    virtual void Release() = 0;
};

// IDatabaseChannel — 数据库通道
// 作为工厂创建 Reader/Writer，内部管理连接
interface IDatabaseChannel : public IChannel {
    // 创建读取器，执行 query 并流式返回结果
    virtual int CreateReader(const char* query, IBatchReader** reader) = 0;

    // 创建写入器，指定目标表
    virtual int CreateWriter(const char* table, IBatchWriter** writer) = 0;

    // 测试连接是否可用
    virtual bool IsConnected() = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_CHANNEL_H_
