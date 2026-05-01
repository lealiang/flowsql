/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_MODEL_EVENT_CALENDAR_SPEC_H_
#define _FLOWSQL_PLUGINS_BASELINE_MODEL_EVENT_CALENDAR_SPEC_H_

#include <cstdint>
#include <string>
#include <vector>

namespace flowsql {
namespace baseline {

struct EventCalendarEntry {
    std::string event_code;
    std::string alignment_mode;
    int64_t start_ts = 0;
    int64_t end_ts = 0;
    bool enabled = true;
    std::string tz;
};

struct EventCalendarSpec {
    std::string calendar_id;
    std::string calendar_version;
    std::vector<EventCalendarEntry> entries;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_MODEL_EVENT_CALENDAR_SPEC_H_
