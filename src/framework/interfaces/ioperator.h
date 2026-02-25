#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IOPERATOR_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IOPERATOR_H_

#include <common/loader.hpp>

#include <string>

#include "idataframe.h"

namespace flowsql {

// {0xd4e5f6a7-bcde-f012-3456-789abcdef012}
const Guid IID_OPERATOR = {0xd4e5f6a7, 0xbcde, 0xf012, {0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x12}};

enum class OperatorPosition : int32_t {
    STORAGE = 0,
    DATA = 1
};

interface IOperator : public IPlugin {
    // 元数据
    virtual std::string Catelog() = 0;
    virtual std::string Name() = 0;
    virtual std::string Description() = 0;
    virtual OperatorPosition Position() = 0;

    // 统一处理接口
    virtual int Work(IDataFrame* in, IDataFrame* out) = 0;

    // 配置
    virtual int Configure(const char* key, const char* value) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IOPERATOR_H_
