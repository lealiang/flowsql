/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_CONFIG_PARSER_H_
#define _FLOWSQL_PLUGINS_BASELINE_CONFIG_PARSER_H_

#include <string>

#include "model/task_spec.h"

namespace flowsql {
namespace baseline {

class ConfigParser {
 public:
    static int ParseValueTask(const char* config_json,
                              BaselineTaskSpec* out,
                              std::string* err);
    static int ParseRatioTask(const char* config_json,
                              BaselineTaskSpec* out,
                              std::string* err);
    static int ParseRelationTask(const char* config_json,
                                 RelationTaskSpec* out,
                                 std::string* err);
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_CONFIG_PARSER_H_
