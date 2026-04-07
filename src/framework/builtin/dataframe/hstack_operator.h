/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_BUILTIN_DATAFRAME_HSTACK_OPERATOR_H_
#define _FLOWSQL_FRAMEWORK_BUILTIN_DATAFRAME_HSTACK_OPERATOR_H_

#include <framework/interfaces/ioperator.h>

#include <string>

namespace flowsql {

// HstackOperator：按列拼接多个 DataFrame（行数必须一致）
class HstackOperator : public IOperator {
 public:
    HstackOperator() = default;
    ~HstackOperator() override = default;

    std::string Category() override { return "builtin"; }
    std::string Name() override { return "hstack"; }
    std::string Description() override { return "Concatenate multiple DataFrames by columns"; }
    OperatorPosition Position() override { return OperatorPosition::DATA; }

    int Work(IChannel* in, IChannel* out) override;
    int Work(Span<IChannel*> inputs, IChannel* out) override;
    int Configure(const char* key, const char* value) override;
    std::string LastError() override { return last_error_; }

 private:
    std::string last_error_;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_BUILTIN_DATAFRAME_HSTACK_OPERATOR_H_
