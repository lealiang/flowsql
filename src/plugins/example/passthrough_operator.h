#ifndef _FLOWSQL_PLUGINS_EXAMPLE_PASSTHROUGH_OPERATOR_H_
#define _FLOWSQL_PLUGINS_EXAMPLE_PASSTHROUGH_OPERATOR_H_

#include <framework/interfaces/ioperator.h>

#include <string>

namespace flowsql {

class PassthroughOperator : public IOperator {
 public:
    PassthroughOperator() = default;
    ~PassthroughOperator() override = default;

    // IPlugin
    int Load() override { return 0; }
    int Unload() override { return 0; }

    // IOperator 元数据
    std::string Catelog() override { return "example"; }
    std::string Name() override { return "passthrough"; }
    std::string Description() override { return "Passthrough operator for testing"; }
    OperatorPosition Position() override { return OperatorPosition::DATA; }

    // 统一处理接口
    int Work(IDataFrame* in, IDataFrame* out) override;

    // 配置
    int Configure(const char*, const char*) override { return 0; }
};

}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_EXAMPLE_PASSTHROUGH_OPERATOR_H_
