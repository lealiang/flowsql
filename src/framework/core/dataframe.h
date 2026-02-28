#ifndef _FLOWSQL_FRAMEWORK_CORE_DATAFRAME_H_
#define _FLOWSQL_FRAMEWORK_CORE_DATAFRAME_H_

#include <arrow/api.h>

#include <memory>
#include <string>
#include <vector>

#include "framework/interfaces/idataframe.h"

namespace flowsql {

// DataType → Arrow 类型映射
std::shared_ptr<arrow::DataType> ToArrowType(DataType type);
// Arrow 类型 → DataType 映射
DataType FromArrowType(const std::shared_ptr<arrow::DataType>& type);

class DataFrame : public IDataFrame {
 public:
    DataFrame() = default;
    ~DataFrame() override = default;

    // Schema
    std::vector<Field> GetSchema() const override;
    void SetSchema(const std::vector<Field>& schema) override;

    // 行操作
    int32_t RowCount() const override;
    int AppendRow(const std::vector<FieldValue>& row) override;
    std::vector<FieldValue> GetRow(int32_t index) const override;

    // 列操作
    std::vector<FieldValue> GetColumn(const std::string& name) const override;

    // IDataEntity 互操作
    int AppendEntity(IDataEntity* entity) override;
    std::shared_ptr<IDataEntity> GetEntity(int32_t index) const override;

    // Arrow 互操作
    std::shared_ptr<arrow::RecordBatch> ToArrow() const override;
    void FromArrow(std::shared_ptr<arrow::RecordBatch> batch) override;

    // 序列化
    std::string ToJson() const override;
    bool FromJson(const std::string& json) override;

    // 清空
    void Clear() override;

 private:
    void Finalize() const;
    void InitBuilders() const;
    void AppendValueToBuilder(int col, const FieldValue& value);
    FieldValue ExtractValue(const std::shared_ptr<arrow::Array>& array, int row) const;

    std::shared_ptr<arrow::Schema> arrow_schema_;
    mutable std::shared_ptr<arrow::RecordBatch> batch_;
    mutable std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders_;
    std::vector<Field> schema_;
    mutable int32_t pending_rows_ = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_DATAFRAME_H_
