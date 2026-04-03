#ifndef _FLOWSQL_FRAMEWORK_BUILTIN_DATAFRAME_PASSTHROUGH_OPERATOR_H_
#define _FLOWSQL_FRAMEWORK_BUILTIN_DATAFRAME_PASSTHROUGH_OPERATOR_H_

#include <framework/interfaces/ioperator.h>

#include <string>

namespace flowsql {

// PassthroughOperator — 无状态直通算子
class PassthroughOperator : public IOperator {
 public:
    PassthroughOperator() = default;
    ~PassthroughOperator() override = default;

    std::string Category() override { return "builtin"; }
    std::string Name() override { return "passthrough"; }
    std::string Description() override { return "Passthrough operator, copies data as-is"; }
    OperatorPosition Position() override { return OperatorPosition::DATA; }

    int Work(IChannel* in, IChannel* out) override;
    int Configure(const char* key, const char* value) override;

 private:
    int delay_ms_ = 0;
    bool force_fail_ = false;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_BUILTIN_DATAFRAME_PASSTHROUGH_OPERATOR_H_
