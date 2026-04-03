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

interface IBlockStreamOperator {
    virtual ~IBlockStreamOperator() = default;

    virtual std::string Category() = 0;
    virtual std::string Name() = 0;
    virtual std::string Description() = 0;

    virtual int Configure(const char* key, const char* value) = 0;
    virtual int Init(const char* with_params_json) = 0;
    virtual int OnSchemaReady(std::shared_ptr<arrow::Schema> schema) = 0;
    virtual int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& block, int64_t ts_ms) = 0;
    virtual int Flush() = 0;
    virtual std::string LastError() { return ""; }
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_STREAM_OPERATOR_H_
