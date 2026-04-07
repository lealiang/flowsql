/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_SERVICES_TASK_SQL_UTILS_H_
#define _FLOWSQL_SERVICES_TASK_SQL_UTILS_H_

#include <rapidjson/document.h>

#include <cstdint>
#include <string>
#include <vector>

namespace flowsql {
namespace task {

std::string TruncateSql(const std::string& sql);
std::string TruncateSummary(const std::string& sql);
std::string BuildSqlsJson(const std::vector<std::string>& sqls);
bool ParseSqlsJson(const std::string& sqls_json, std::vector<std::string>* sqls);
bool IsDataFrameRef(const std::string& name);
std::string DataFrameNamePart(const std::string& full_name);
std::string ExtractStageFromErrorMessage(const std::string& error_message);
std::string BuildOperatorChainFromSql(const std::string& sql);
void ParseRuntimeErrorCode(const rapidjson::Value& value,
                           std::string* error_code_out,
                           int* error_no_out);

}  // namespace task
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_TASK_SQL_UTILS_H_
