/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_STREAM_OPERATOR_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_STREAM_OPERATOR_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <memory>
#include <string>

namespace arrow {
class RecordBatch;
class Schema;
}

namespace flowsql {

// {0x89abcdef-0123-4567-89ab-cdef01234567}
const Guid IID_BLOCK_STREAM_OPERATOR = {0x89abcdef, 0x0123, 0x4567,
                                        {0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67}};

/**
 * @brief 块式流算子接口。
 */
interface IBlockStreamOperator {
    virtual ~IBlockStreamOperator() = default;

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
     * @brief 配置静态参数。
     * @param key 配置键。
     * @param value 配置值。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Configure(const char* key, const char* value) = 0;
    /**
     * @brief 任务级初始化。
     * @param with_params_json WITH 参数 JSON 字符串。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Init(const char* with_params_json) = 0;
    /**
     * @brief Schema 就绪回调。
     * @param schema 输入 schema。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int OnSchemaReady(std::shared_ptr<arrow::Schema> schema) = 0;
    /**
     * @brief 处理单个数据块。
     * @param block 输入数据块。
     * @param ts_ms 时间戳（毫秒）。
     * @return 0 表示继续，1 表示主动停止，<0 表示错误。
     */
    virtual int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& block, int64_t ts_ms) = 0;
    /**
     * @brief 流结束后执行收尾刷新。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Flush() = 0;
    /**
     * @brief 获取最近一次错误信息。
     * @return 错误字符串；无错误时可返回空串。
     */
    virtual std::string LastError() { return ""; }
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_STREAM_OPERATOR_H_
