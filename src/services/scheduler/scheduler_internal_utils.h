/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_SCHEDULER_SCHEDULER_INTERNAL_UTILS_H_
#define _FLOWSQL_SCHEDULER_SCHEDULER_INTERNAL_UTILS_H_

#include <rapidjson/document.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace flowsql {
namespace scheduler {

std::string ToLowerAscii(std::string s);
bool IEquals(const std::string& a, const std::string& b);
bool StartsWithIgnoreCase(const std::string& s, const std::string& prefix);

bool IsDataframeRefName(const std::string& name);
std::string DataframeNamePart(const std::string& name);
bool IsStreamRefName(const std::string& name);
std::string StreamNamePart(const std::string& name);

struct ParsedChannelRef {
    std::string raw;
    std::string base;
    bool has_selector = false;
    bool wildcard_selector = false;
    int selector_index = -1;
};

bool ParseChannelRef(const std::string& raw, ParsedChannelRef* out, std::string* err);
bool ParseDatabaseDestination(const std::string& dest,
                              std::string* db_type,
                              std::string* db_name,
                              std::string* table_name);
bool IsQualifiedDestination(const std::string& dest);

int32_t MapStreamManagerErrorToStatus(int rc);
std::string MakeStreamChannelKey(const std::string& type, const std::string& name);
std::string NormalizeStreamRole(const std::string& role);
bool IsSourceRoleAllowed(const std::string& role);
bool IsSinkRoleAllowed(const std::string& role);

int ParseOptionObject(const std::string& option, rapidjson::Document* out, std::string* err);
std::string OptionObjectToJson(const rapidjson::Value& option_obj);
std::string BuildOptionWithRoleJson(const rapidjson::Value* options, const std::string& role);
std::string ReadRoleFromOption(const std::string& option);

std::string ExtractStageFromExecutionError(const std::string& error);
int64_t CurrentTimeMs();
size_t NextPowerOfTwo(size_t value);
std::string TrimAsciiSpace(const std::string& input);
std::string ExtractErrorMessage(const std::string& json);

}  // namespace scheduler
}  // namespace flowsql

#endif  // _FLOWSQL_SCHEDULER_SCHEDULER_INTERNAL_UTILS_H_
