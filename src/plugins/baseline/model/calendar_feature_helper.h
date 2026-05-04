/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_MODEL_CALENDAR_FEATURE_HELPER_H_
#define _FLOWSQL_PLUGINS_BASELINE_MODEL_CALENDAR_FEATURE_HELPER_H_

#include <cstdint>
#include <ctime>
#include <string>

namespace flowsql {
namespace baseline {

struct LocalCalendarFeature {
    bool valid = false;
    int64_t bucket_id = 0;
    int64_t epoch_second = 0;

    int32_t hour = 0;
    int32_t minute = 0;
    int32_t second = 0;
    int32_t weekday = 0;
    int32_t monday_weekday = 0;
    int32_t day_of_month = 0;
    int32_t days_to_month_end = 0;
    bool is_last_weekday_of_month = false;

    int32_t second_of_day = 0;
    int32_t second_of_week = 0;
    int64_t local_wall_second = 0;
    double day_phase = 0.0;
    double week_phase = 0.0;
};

// Calendar feature helper 是 bucket_id -> 本地日历语义的唯一入口。
// bucket_id 始终按 UTC 绝对窗口解释；tz 只影响本地相位和月位置字段。
bool ResolveOneLocalCalendarFeature(int64_t bucket_id,
                                    int64_t delta,
                                    const std::string& tz,
                                    LocalCalendarFeature* out);
bool ResolveLocalTime(int64_t bucket_id, int64_t delta, const std::string& tz, std::tm* out);
double PhaseDayLocal(int64_t bucket_id, int64_t delta, const std::string& tz);
double PhaseWeekLocal(int64_t bucket_id, int64_t delta, const std::string& tz);
int32_t DayOfMonthLocal(int64_t bucket_id, int64_t delta, const std::string& tz);
int32_t DaysToMonthEndLocal(int64_t bucket_id, int64_t delta, const std::string& tz);
bool IsLastWeekdayOfMonthLocal(int64_t bucket_id, int64_t delta, const std::string& tz);
int64_t LocalWallClockSecond(int64_t utc_epoch_second, const std::string& tz);

inline double phase_day_local(int64_t bucket_id, int64_t delta, const std::string& tz) {
    return PhaseDayLocal(bucket_id, delta, tz);
}

inline double phase_week_local(int64_t bucket_id, int64_t delta, const std::string& tz) {
    return PhaseWeekLocal(bucket_id, delta, tz);
}

inline int32_t day_of_month(int64_t bucket_id, int64_t delta, const std::string& tz) {
    return DayOfMonthLocal(bucket_id, delta, tz);
}

inline int32_t days_to_month_end(int64_t bucket_id, int64_t delta, const std::string& tz) {
    return DaysToMonthEndLocal(bucket_id, delta, tz);
}

inline bool is_last_weekday_of_month(int64_t bucket_id, int64_t delta, const std::string& tz) {
    return IsLastWeekdayOfMonthLocal(bucket_id, delta, tz);
}

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_MODEL_CALENDAR_FEATURE_HELPER_H_
