/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/model/event_calendar_matcher.h"

#include <common/error_code.h>

#include <algorithm>
#include <unordered_map>

#include "plugins/baseline/model/calendar_feature_helper.h"

namespace flowsql {
namespace baseline {

namespace {

bool IsAllowedScope(const std::string& scope_type) {
    return scope_type == "global" || scope_type == "feature" || scope_type == "key" ||
           scope_type == "key_feature";
}

bool IsAllowedAlignment(const std::string& alignment_mode) {
    return alignment_mode == "absolute_utc" || alignment_mode == "local_wall_clock";
}

bool ScopeMatches(const CompiledEventCalendarEntry& entry, const BaselineTaskSpec& task_spec) {
    if (entry.scope_type == "global") return true;
    if (entry.scope_type == "feature") return entry.feature == task_spec.feature;
    if (entry.scope_type == "key") return entry.key == task_spec.key;
    if (entry.scope_type == "key_feature") {
        return entry.key == task_spec.key && entry.feature == task_spec.feature;
    }
    return false;
}

bool IntervalsOverlap(int64_t lhs_start, int64_t lhs_end, int64_t rhs_start, int64_t rhs_end) {
    return lhs_start < rhs_end && rhs_start < lhs_end;
}

bool EntryOverlapsBucket(const CompiledEventCalendarEntry& entry,
                         const BaselineTaskSpec& task_spec,
                         int64_t bucket_id) {
    const int64_t bucket_start_utc = bucket_id * task_spec.delta;
    const int64_t bucket_end_utc = bucket_start_utc + task_spec.delta;
    if (entry.alignment_mode == "absolute_utc") {
        return IntervalsOverlap(bucket_start_utc, bucket_end_utc, entry.start_ts, entry.end_ts);
    }

    const std::string& event_tz = entry.tz.empty() ? task_spec.tz : entry.tz;
    const int64_t bucket_start_local = LocalWallClockSecond(bucket_start_utc, event_tz);
    const int64_t bucket_end_local = LocalWallClockSecond(bucket_end_utc, event_tz);
    const int64_t event_start_local = LocalWallClockSecond(entry.start_ts, event_tz);
    const int64_t event_end_local = LocalWallClockSecond(entry.end_ts, event_tz);
    return IntervalsOverlap(bucket_start_local, bucket_end_local, event_start_local, event_end_local);
}

}  // namespace

int CompileEventCalendar(const EventCalendarSpec& spec,
                         const BaselineTaskSpec&,
                         CompiledEventCalendar* out,
                         std::string* err) {
    if (!out) return error::BAD_REQUEST;
    *out = CompiledEventCalendar{};
    if (spec.calendar_id.empty() || spec.calendar_version.empty()) {
        if (err) *err = "calendar_id and calendar_version must not be empty";
        return error::BAD_REQUEST;
    }

    out->calendar_id = spec.calendar_id;
    out->calendar_version = spec.calendar_version;
    std::unordered_map<std::string, std::size_t> code_index_by_name;

    for (const auto& entry : spec.entries) {
        if (!entry.enabled) continue;
        if (entry.event_code.empty()) {
            if (err) *err = "event_code must not be empty";
            return error::BAD_REQUEST;
        }
        if (!IsAllowedScope(entry.scope_type)) {
            if (err) *err = "scope_type is invalid";
            return error::BAD_REQUEST;
        }
        if (!IsAllowedAlignment(entry.alignment_mode)) {
            if (err) *err = "alignment_mode is invalid";
            return error::BAD_REQUEST;
        }
        if (entry.end_ts <= entry.start_ts) {
            if (err) *err = "event interval must be positive";
            return error::BAD_REQUEST;
        }
        if ((entry.scope_type == "feature" || entry.scope_type == "key_feature") &&
            entry.feature.empty()) {
            if (err) *err = "feature scoped event must set feature";
            return error::BAD_REQUEST;
        }
        if ((entry.scope_type == "key" || entry.scope_type == "key_feature") && entry.key.empty()) {
            if (err) *err = "key scoped event must set key";
            return error::BAD_REQUEST;
        }
        if (entry.alignment_mode == "local_wall_clock" && entry.tz.empty()) {
            if (err) *err = "local_wall_clock event must set tz";
            return error::BAD_REQUEST;
        }

        auto code_it = code_index_by_name.find(entry.event_code);
        if (code_it == code_index_by_name.end()) {
            const std::size_t next_index = out->enabled_event_codes.size();
            out->enabled_event_codes.push_back(entry.event_code);
            code_it = code_index_by_name.emplace(entry.event_code, next_index).first;
        }

        CompiledEventCalendarEntry compiled;
        compiled.event_code = entry.event_code;
        compiled.scope_type = entry.scope_type;
        compiled.alignment_mode = entry.alignment_mode;
        compiled.start_ts = entry.start_ts;
        compiled.end_ts = entry.end_ts;
        compiled.feature = entry.feature;
        compiled.key = entry.key;
        compiled.tz = entry.tz;
        compiled.event_code_index = code_it->second;
        out->entries.push_back(std::move(compiled));
    }

    return error::OK;
}

std::vector<std::string> ResolveBucketEvents(const CompiledEventCalendar& calendar,
                                             const BaselineTaskSpec& task_spec,
                                             int64_t bucket_id) {
    std::vector<uint8_t> hit(calendar.enabled_event_codes.size(), 0);
    for (const auto& entry : calendar.entries) {
        if (!ScopeMatches(entry, task_spec)) continue;
        if (!EntryOverlapsBucket(entry, task_spec, bucket_id)) continue;
        if (entry.event_code_index < hit.size()) {
            hit[entry.event_code_index] = 1;
        }
    }

    std::vector<std::string> events;
    for (std::size_t i = 0; i < hit.size(); ++i) {
        if (hit[i]) events.push_back(calendar.enabled_event_codes[i]);
    }
    return events;
}

int BuildEventIndicatorRow(const CompiledEventCalendar& calendar,
                           const BaselineTaskSpec& task_spec,
                           int64_t bucket_id,
                           double* out_row,
                           std::size_t row_size) {
    if (!out_row || row_size < calendar.enabled_event_codes.size()) return error::BAD_REQUEST;
    std::fill(out_row, out_row + row_size, 0.0);
    for (const auto& entry : calendar.entries) {
        if (!ScopeMatches(entry, task_spec)) continue;
        if (!EntryOverlapsBucket(entry, task_spec, bucket_id)) continue;
        if (entry.event_code_index < row_size) {
            out_row[entry.event_code_index] = 1.0;
        }
    }
    return error::OK;
}

}  // namespace baseline
}  // namespace flowsql
