#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IDATA_ENTITY_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IDATA_ENTITY_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace flowsql {

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

struct Field {
    std::string name;
    DataType type;
    int32_t size = 0;
    std::string description;
};

// {0xa1b2c3d4-1234-5678-9abc-def012345678}
const Guid IID_DATA_ENTITY = {0xa1b2c3d4, 0x1234, 0x5678, {0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78}};

interface IDataEntity {
    virtual ~IDataEntity() = default;

    virtual std::string GetEntityType() const = 0;
    virtual std::vector<Field> GetSchema() const = 0;
    virtual bool HasField(const std::string& name) const = 0;
    virtual FieldValue GetFieldValue(const std::string& name) const = 0;
    virtual void SetFieldValue(const std::string& name, const FieldValue& value) = 0;

    virtual std::string ToJson() const = 0;
    virtual bool FromJson(const std::string& json) = 0;
    virtual std::shared_ptr<IDataEntity> Clone() const = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IDATA_ENTITY_H_
