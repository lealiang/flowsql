/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

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

/**
 * @brief 流式算子接口，定义流任务生命周期与数据处理回调。
 */
interface IStreamOperator {
    virtual ~IStreamOperator() = default;

    /**
     * @brief 获取算子分类。
     * @return 分类字符串。
     */
    virtual std::string Category() = 0;
    /**
     * @brief 获取算子名称。
     * @return 名称字符串。
     */
    virtual std::string Name() = 0;
    /**
     * @brief 获取算子描述。
     * @return 描述字符串。
     */
    virtual std::string Description() = 0;

    /**
     * @brief 配置静态参数（插件加载阶段调用）。
     * @param key 配置键。
     * @param value 配置值。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Configure(const char* key, const char* value) = 0;

    /**
     * @brief 任务级初始化（每次执行任务调用）。
     * @param with_params_json WITH 参数 JSON。
     * @param sink_ctx sink 通道上下文。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Init(const char* with_params_json, const StreamSinkContext& sink_ctx) = 0;

    /**
     * @brief 输入 schema 就绪回调。
     * @param schema 输入 schema。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int OnSchemaReady(std::shared_ptr<arrow::Schema> schema) = 0;

    /**
     * @brief 处理单个输入批次。
     * @param batch 输入批次。
     * @param ts_ms 批次时间戳（毫秒）。
     * @return 0 表示继续，1 表示算子主动停止，<0 表示错误。
     */
    virtual int Process(const arrow::RecordBatch& batch, int64_t ts_ms) = 0;

    /**
     * @brief 超时驱动回调（无输入时触发）。
     * @param current_ms 当前时间戳（毫秒）。
     * @return 0 表示继续，1 表示主动停止，<0 表示错误。
     */
    virtual int Tick(int64_t current_ms) = 0;

    /**
     * @brief 流结束后的收尾刷新。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Flush() = 0;

    /**
     * @brief 获取运行统计信息。
     * @return 统计信息字符串（通常为 JSON）。
     */
    virtual std::string GetStats() = 0;
    /**
     * @brief 获取最近一次错误信息。
     * @return 错误字符串；无错误时可返回空串。
     */
    virtual std::string LastError() { return ""; }

    /**
     * @brief 声明算子的并行策略。
     * @return 并行策略枚举值。
     */
    virtual ParallelStrategy GetParallelStrategy() const {
        return ParallelStrategy::NONE;
    }
    /**
     * @brief 声明分区规格字符串（KEYED 策略使用）。
     * @return 分区规格字符串。
     */
    virtual std::string GetPartitionSpec() const { return ""; }
    /**
     * @brief 声明并行度。
     * @return 并行 worker 数量。
     */
    virtual int GetParallelism() const { return 1; }
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_OPERATOR_H_
