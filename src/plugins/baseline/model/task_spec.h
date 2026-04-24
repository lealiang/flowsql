/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_MODEL_TASK_SPEC_H_
#define _FLOWSQL_PLUGINS_BASELINE_MODEL_TASK_SPEC_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "event_calendar_spec.h"
#include "series_override.h"

namespace flowsql {
namespace baseline {

struct BaselineTaskSpec {
    std::string name;
    std::string key;
    std::string feature;
    std::string feature_type;
    std::string feature_profile;
    int64_t delta = 0;
    std::string tz;
    std::vector<SeriesBaselineSourceConfig> baseline_source_configs;
    std::optional<EventCalendarSpec> event_calendar_spec;
    std::string config_json;
};

struct RelationSupportPolicySpec {
    int32_t k_support = 0;
    double min_hist_share = 0.0;
    double min_active_ratio = 0.0;
};

struct RelationSummaryPolicySpec {
    int32_t k_head = 0;
    int32_t k_stable = 0;
};

struct RelationTaskSpec {
    std::string task_id;
    std::string name;
    std::string feature_base;
    std::string group_space_id;
    std::optional<std::string> group_space_version;
    std::string metric_set_id;
    std::vector<std::string> metrics;
    std::string encode_type;
    RelationSupportPolicySpec support_policy;
    RelationSummaryPolicySpec summary_policy;
    std::string config_json;
};

struct RelationTaskClockSpec {
    int64_t delta = 0;
    std::string tz;
};

struct RelationTaskCreateSpec {
    RelationTaskSpec task_spec;
    RelationTaskClockSpec clock_spec;
    std::optional<EventCalendarSpec> event_calendar_spec;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_MODEL_TASK_SPEC_H_
