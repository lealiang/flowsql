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

interface IDataFrame {
    virtual ~IDataFrame() = default;

    // Schema
    virtual std::vector<Field> GetSchema() const = 0;
    virtual void SetSchema(const std::vector<Field>& schema) = 0;

    // 行操作
    virtual int32_t RowCount() const = 0;
    virtual int AppendRow(const std::vector<FieldValue>& row) = 0;
    virtual std::vector<FieldValue> GetRow(int32_t index) const = 0;

    // 列操作
    virtual std::vector<FieldValue> GetColumn(const std::string& name) const = 0;

    // Arrow 互操作（零拷贝）
    virtual std::shared_ptr<arrow::RecordBatch> ToArrow() const = 0;
    virtual void FromArrow(std::shared_ptr<arrow::RecordBatch> batch) = 0;

    // 序列化
    virtual std::string ToJson() const = 0;
    virtual bool FromJson(const std::string& json) = 0;

    // 清空
    virtual void Clear() = 0;

    // 按条件过滤行（返回新的过滤后 DataFrame）
    // condition 格式: "column=value", "column>value", "column<value" 等
    // 返回: 0=成功, <0=错误（列不存在等）
    virtual int Filter(const char* condition) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IDATAFRAME_H_
