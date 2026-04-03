#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_OPERATOR_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_OPERATOR_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <cstdint>
#include <memory>
#include <string>

namespace arrow {
class RecordBatch;
class Schema;
}

namespace flowsql {

interface IChannel;

// {0xc6d7e8f9-0a1b-2c3d-4e5f-60718293a4b5}
const Guid IID_STREAM_OPERATOR = {0xc6d7e8f9, 0x0a1b, 0x2c3d,
                                  {0x4e, 0x5f, 0x60, 0x71, 0x82, 0x93, 0xa4, 0xb5}};

enum class ParallelStrategy {
    NONE,
    STATELESS,
    KEYED,
};

struct StreamSinkContext {
    IChannel* sink_channel = nullptr;  // non-owning
    std::string sink_type;
    std::string into_raw;
    std::string db_type;
    std::string db_name;
    std::string table_name;
};

interface IStreamOperator {
    virtual ~IStreamOperator() = default;

    // 元数据
    virtual std::string Category() = 0;
    virtual std::string Name() = 0;
    virtual std::string Description() = 0;

    // 静态配置：插件加载时调用
    virtual int Configure(const char* key, const char* value) = 0;

    // 任务级初始化：每次执行任务时调用
    virtual int Init(const char* with_params_json, const StreamSinkContext& sink_ctx) = 0;

    // schema 就绪回调
    virtual int OnSchemaReady(std::shared_ptr<arrow::Schema> schema) = 0;

    // 核心处理（0=继续，1=算子主动停止，<0=错误）
    virtual int Process(const arrow::RecordBatch& batch, int64_t ts_ms) = 0;

    // timeout 驱动
    virtual int Tick(int64_t current_ms) = 0;

    // 流结束清理
    virtual int Flush() = 0;

    // 运行状态
    virtual std::string GetStats() = 0;
    virtual std::string LastError() { return ""; }

    // 并行声明
    virtual ParallelStrategy GetParallelStrategy() const {
        return ParallelStrategy::NONE;
    }
    virtual std::string GetPartitionSpec() const { return ""; }
    virtual int GetParallelism() const { return 1; }
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_OPERATOR_H_
