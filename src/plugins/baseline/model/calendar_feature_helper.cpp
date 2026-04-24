/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/model/calendar_feature_helper.h"

#include <cstdlib>
#include <mutex>
#include <string>

namespace flowsql {
namespace baseline {

namespace {

constexpr int32_t kSecondsPerDay = 24 * 60 * 60;
constexpr int32_t kSecondsPerWeek = 7 * kSecondsPerDay;

std::mutex& TimezoneMutex() {
    static std::mutex mutex;
    return mutex;
}

std::string EffectiveTimezone(const std::string& tz) {
    return tz.empty() ? "UTC" : tz;
}

bool IsUtcTimezone(const std::string& tz) {
    const std::string effective_tz = EffectiveTimezone(tz);
    return effective_tz == "UTC" || effective_tz == "Etc/UTC" || effective_tz == "GMT";
}

bool ResolveLocalTimeFromEpoch(int64_t epoch_second, const std::string& tz, std::tm* out) {
    if (!out) return false;

    const std::time_t epoch = static_cast<std::time_t>(epoch_second);
    if (IsUtcTimezone(tz)) {
        return gmtime_r(&epoch, out) != nullptr;
    }

    // POSIX timezone API 依赖进程级 TZ 环境变量。这里用互斥锁把影响面限制在
    // helper 内部，避免 trainer / detector 分散实现更危险的时区切换逻辑。
    std::lock_guard<std::mutex> guard(TimezoneMutex());
    const char* old_tz = std::getenv("TZ");
    const std::string old_tz_value = old_tz ? old_tz : "";
    const bool had_old_tz = old_tz != nullptr;

    setenv("TZ", EffectiveTimezone(tz).c_str(), 1);
    tzset();
    const bool ok = localtime_r(&epoch, out) != nullptr;

    if (had_old_tz) {
        setenv("TZ", old_tz_value.c_str(), 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
    return ok;
}

int32_t DaysInMonth(int32_t year, int32_t month) {
    static constexpr int32_t kDaysByMonth[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    if (month != 2) return kDaysByMonth[month - 1];
    const bool leap = (year % 400 == 0) || (year % 4 == 0 && year % 100 != 0);
    return leap ? 29 : 28;
}

int32_t PositiveModulo(int32_t value, int32_t mod) {
    const int32_t r = value % mod;
    return r < 0 ? r + mod : r;
}

}  // namespace

bool ResolveLocalTime(int64_t bucket_id, int64_t delta, const std::string& tz, std::tm* out) {
    if (delta <= 0) return false;
    return ResolveLocalTimeFromEpoch(bucket_id * delta, tz, out);
}

double PhaseDayLocal(int64_t bucket_id, int64_t delta, const std::string& tz) {
    std::tm local{};
    if (!ResolveLocalTime(bucket_id, delta, tz, &local)) return 0.0;
    const int32_t second_of_day = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    return static_cast<double>(second_of_day) / static_cast<double>(kSecondsPerDay);
}

double PhaseWeekLocal(int64_t bucket_id, int64_t delta, const std::string& tz) {
    std::tm local{};
    if (!ResolveLocalTime(bucket_id, delta, tz, &local)) return 0.0;
    const int32_t monday_based_wday = PositiveModulo(local.tm_wday + 6, 7);
    const int32_t second_of_day = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    const int32_t second_of_week = monday_based_wday * kSecondsPerDay + second_of_day;
    return static_cast<double>(second_of_week) / static_cast<double>(kSecondsPerWeek);
}

int32_t DayOfMonthLocal(int64_t bucket_id, int64_t delta, const std::string& tz) {
    std::tm local{};
    if (!ResolveLocalTime(bucket_id, delta, tz, &local)) return 0;
    return local.tm_mday;
}

int32_t DaysToMonthEndLocal(int64_t bucket_id, int64_t delta, const std::string& tz) {
    std::tm local{};
    if (!ResolveLocalTime(bucket_id, delta, tz, &local)) return 0;
    const int32_t year = local.tm_year + 1900;
    const int32_t month = local.tm_mon + 1;
    return DaysInMonth(year, month) - local.tm_mday;
}

bool IsLastWeekdayOfMonthLocal(int64_t bucket_id, int64_t delta, const std::string& tz) {
    std::tm local{};
    if (!ResolveLocalTime(bucket_id, delta, tz, &local)) return false;
    const int32_t year = local.tm_year + 1900;
    const int32_t month = local.tm_mon + 1;
    return local.tm_mday + 7 > DaysInMonth(year, month);
}

int64_t LocalWallClockSecond(int64_t utc_epoch_second, const std::string& tz) {
    std::tm local{};
    if (!ResolveLocalTimeFromEpoch(utc_epoch_second, tz, &local)) return utc_epoch_second;
    local.tm_isdst = -1;
    return static_cast<int64_t>(timegm(&local));
}

}  // namespace baseline
}  // namespace flowsql
