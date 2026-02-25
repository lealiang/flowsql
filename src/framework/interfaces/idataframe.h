#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IDATAFRAME_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IDATAFRAME_H_

#include <arrow/api.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "idata_entity.h"

namespace flowsql {

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

    // IDataEntity 互操作
    virtual int AppendEntity(IDataEntity* entity) = 0;
    virtual std::shared_ptr<IDataEntity> GetEntity(int32_t index) const = 0;

    // Arrow 互操作（零拷贝）
    virtual std::shared_ptr<arrow::RecordBatch> ToArrow() const = 0;
    virtual void FromArrow(std::shared_ptr<arrow::RecordBatch> batch) = 0;

    // 序列化
    virtual std::string ToJson() const = 0;
    virtual bool FromJson(const std::string& json) = 0;

    // 清空
    virtual void Clear() = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IDATAFRAME_H_
