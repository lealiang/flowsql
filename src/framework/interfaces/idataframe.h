/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IDATAFRAME_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IDATAFRAME_H_

#include <arrow/api.h>
#include <common/typedef.h>

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace flowsql {

// 数据类型枚举
enum class DataType : int32_t {
    INT32 = 0,
    INT64,
    UINT32,
    UINT64,
    FLOAT,
    DOUBLE,
    STRING,
    BYTES,
    TIMESTAMP,
    BOOLEAN
};

// 字段值（variant 类型）
using FieldValue = std::variant<int32_t,               // INT32
                                int64_t,               // INT64
                                uint32_t,              // UINT32
                                uint64_t,              // UINT64
                                float,                 // FLOAT
                                double,                // DOUBLE
                                std::string,           // STRING
                                std::vector<uint8_t>,  // BYTES
                                bool                   // BOOLEAN (TIMESTAMP 复用 INT64)
                                >;

// 字段描述
struct Field {
    std::string name;
    DataType type;
    int32_t size = 0;
    std::string description;
};

/**
 * @brief 列式内存数据结构接口，提供 Schema、行列访问、序列化与 Arrow 互操作能力。
 */
interface IDataFrame {
    virtual ~IDataFrame() = default;

    /**
     * @brief 获取当前数据框 schema。
     * @return 字段描述数组。
     */
    virtual std::vector<Field> GetSchema() const = 0;
    /**
     * @brief 设置当前数据框 schema。
     * @param schema 字段描述数组。
     */
    virtual void SetSchema(const std::vector<Field>& schema) = 0;

    /**
     * @brief 获取当前行数。
     * @return 行数。
     */
    virtual int32_t RowCount() const = 0;
    /**
     * @brief 追加一行数据。
     * @param row 行字段值数组，顺序与 schema 一致。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int AppendRow(const std::vector<FieldValue>& row) = 0;
    /**
     * @brief 按行号读取一行数据。
     * @param index 行索引（从 0 开始）。
     * @return 行字段值数组；越界时实现可返回空数组。
     */
    virtual std::vector<FieldValue> GetRow(int32_t index) const = 0;

    /**
     * @brief 按列名读取一列数据。
     * @param name 列名。
     * @return 列字段值数组；列不存在时实现可返回空数组。
     */
    virtual std::vector<FieldValue> GetColumn(const std::string& name) const = 0;

    /**
     * @brief 导出为 Arrow RecordBatch。
     * @return Arrow RecordBatch 智能指针。
     */
    virtual std::shared_ptr<arrow::RecordBatch> ToArrow() const = 0;
    /**
     * @brief 从 Arrow RecordBatch 导入数据。
     * @param batch Arrow RecordBatch 智能指针。
     */
    virtual void FromArrow(std::shared_ptr<arrow::RecordBatch> batch) = 0;

    /**
     * @brief 序列化为 JSON 字符串。
     * @return JSON 字符串。
     */
    virtual std::string ToJson() const = 0;
    /**
     * @brief 从 JSON 字符串反序列化。
     * @param json 输入 JSON 字符串。
     * @return true 表示成功，false 表示失败。
     */
    virtual bool FromJson(const std::string& json) = 0;

    /**
     * @brief 清空所有数据与状态。
     */
    virtual void Clear() = 0;

    /**
     * @brief 按条件过滤当前数据框。
     * @param condition 过滤条件表达式，例如 "column=value"、"column>value"。
     * @return 0 表示成功，非 0 表示失败（如列不存在、表达式错误）。
     */
    virtual int Filter(const char* condition) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IDATAFRAME_H_
