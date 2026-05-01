/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_MODEL_EVENT_CALENDAR_MATCHER_H_
#define _FLOWSQL_PLUGINS_BASELINE_MODEL_EVENT_CALENDAR_MATCHER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "plugins/baseline/model/event_calendar_spec.h"
#include "plugins/baseline/model/task_spec.h"

namespace flowsql {
namespace baseline {

struct CompiledEventCalendarEntry {
    std::string event_code;
    std::string alignment_mode;
    int64_t start_ts = 0;
    int64_t end_ts = 0;
    std::string tz;
    std::size_t event_code_index = 0;
};

struct CompiledEventCalendar {
    std::string calendar_id;
    std::string calendar_version;
    std::vector<std::string> enabled_event_codes;
    std::vector<CompiledEventCalendarEntry> entries;
};

int CompileEventCalendar(const EventCalendarSpec& spec,
                         const BaselineTaskSpec& task_spec,
                         CompiledEventCalendar* out,
                         std::string* err);

std::vector<std::string> ResolveBucketEvents(const CompiledEventCalendar& calendar,
                                             const BaselineTaskSpec& task_spec,
                                             int64_t bucket_id);

int BuildEventIndicatorRow(const CompiledEventCalendar& calendar,
                           const BaselineTaskSpec& task_spec,
                           int64_t bucket_id,
                           double* out_row,
                           std::size_t row_size);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_MODEL_EVENT_CALENDAR_MATCHER_H_
