/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/model/calendar_feature_helper.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <unicode/ucal.h>
#include <unicode/ustring.h>

namespace flowsql {
namespace baseline {

namespace {

constexpr int32_t kSecondsPerDay = 24 * 60 * 60;
constexpr int32_t kSecondsPerWeek = 7 * kSecondsPerDay;

std::string EffectiveTimezone(const std::string& tz) {
    return tz.empty() ? "UTC" : tz;
}

bool IsUtcTimezone(const std::string& tz) {
    const std::string effective_tz = EffectiveTimezone(tz);
    return effective_tz == "UTC" || effective_tz == "Etc/UTC" || effective_tz == "GMT";
}

struct CalendarCloser {
    void operator()(UCalendar* calendar) const {
        if (calendar) ucal_close(calendar);
    }
};

using CalendarPtr = std::unique_ptr<UCalendar, CalendarCloser>;

std::vector<UChar> ToUCharBuffer(const std::string& text) {
    std::vector<UChar> buffer(text.size() + 1, 0);
    u_charsToUChars(text.c_str(), buffer.data(), static_cast<int32_t>(buffer.size()));
    return buffer;
}

bool CanonicalizeTimezoneId(const std::string& tz, std::string* out) {
    if (!out) return false;
    const std::string effective_tz = EffectiveTimezone(tz);
    if (IsUtcTimezone(effective_tz)) {
        *out = "UTC";
        return true;
    }

    const std::vector<UChar> src = ToUCharBuffer(effective_tz);
    UErrorCode status = U_ZERO_ERROR;
    UBool is_system_id = false;
    std::vector<UChar> canonical(src.size() + 32, 0);
    int32_t length = ucal_getCanonicalTimeZoneID(src.data(),
                                                 -1,
                                                 canonical.data(),
                                                 static_cast<int32_t>(canonical.size()),
                                                 &is_system_id,
                                                 &status);
    if (status == U_BUFFER_OVERFLOW_ERROR) {
        canonical.assign(static_cast<std::size_t>(length + 1), 0);
        status = U_ZERO_ERROR;
        length = ucal_getCanonicalTimeZoneID(src.data(),
                                             -1,
                                             canonical.data(),
                                             static_cast<int32_t>(canonical.size()),
                                             &is_system_id,
                                             &status);
    }
    if (U_FAILURE(status) || length <= 0) return false;

    out->assign(static_cast<std::size_t>(length), '\0');
    u_UCharsToChars(canonical.data(), out->data(), length);
    return true;
}

CalendarPtr CreateCalendar(const std::string& tz) {
    std::string canonical_tz;
    if (!CanonicalizeTimezoneId(tz, &canonical_tz)) return nullptr;

    const std::vector<UChar> zone_id = ToUCharBuffer(canonical_tz);
    UErrorCode status = U_ZERO_ERROR;
    CalendarPtr calendar(
        ucal_open(zone_id.data(), -1, "en_US_POSIX", UCAL_GREGORIAN, &status));
    if (U_FAILURE(status) || !calendar) return nullptr;
    return calendar;
}

UCalendar* GetThreadLocalCalendar(const std::string& tz) {
    thread_local std::unordered_map<std::string, CalendarPtr> calendars;
    const std::string effective_tz = EffectiveTimezone(tz);
    auto it = calendars.find(effective_tz);
    if (it != calendars.end()) return it->second.get();

    CalendarPtr calendar = CreateCalendar(effective_tz);
    if (!calendar) return nullptr;

    UCalendar* raw = calendar.get();
    calendars.emplace(effective_tz, std::move(calendar));
    return raw;
}

bool FillLocalTime(UCalendar* calendar, int64_t epoch_second, std::tm* out) {
    if (!calendar || !out) return false;

    UErrorCode status = U_ZERO_ERROR;
    ucal_setMillis(calendar, static_cast<UDate>(epoch_second) * 1000.0, &status);
    if (U_FAILURE(status)) return false;

    const int32_t year = ucal_get(calendar, UCAL_YEAR, &status);
    const int32_t month = ucal_get(calendar, UCAL_MONTH, &status);
    const int32_t day = ucal_get(calendar, UCAL_DAY_OF_MONTH, &status);
    const int32_t hour = ucal_get(calendar, UCAL_HOUR_OF_DAY, &status);
    const int32_t minute = ucal_get(calendar, UCAL_MINUTE, &status);
    const int32_t second = ucal_get(calendar, UCAL_SECOND, &status);
    const int32_t weekday = ucal_get(calendar, UCAL_DAY_OF_WEEK, &status);
    const int32_t yearday = ucal_get(calendar, UCAL_DAY_OF_YEAR, &status);
    const int32_t dst_offset = ucal_get(calendar, UCAL_DST_OFFSET, &status);
    if (U_FAILURE(status)) return false;

    *out = std::tm{};
    out->tm_year = year - 1900;
    out->tm_mon = month;
    out->tm_mday = day;
    out->tm_hour = hour;
    out->tm_min = minute;
    out->tm_sec = second;
    out->tm_wday = (weekday + 6) % 7;
    out->tm_yday = yearday - 1;
    out->tm_isdst = dst_offset != 0 ? 1 : 0;
    return true;
}

bool ResolveLocalOffsetSecond(UCalendar* calendar,
                              int64_t epoch_second,
                              int32_t* out_offset_second) {
    if (!calendar || !out_offset_second) return false;

    UErrorCode status = U_ZERO_ERROR;
    ucal_setMillis(calendar, static_cast<UDate>(epoch_second) * 1000.0, &status);
    if (U_FAILURE(status)) return false;

    const int32_t zone_offset = ucal_get(calendar, UCAL_ZONE_OFFSET, &status);
    const int32_t dst_offset = ucal_get(calendar, UCAL_DST_OFFSET, &status);
    if (U_FAILURE(status)) return false;

    *out_offset_second = (zone_offset + dst_offset) / 1000;
    return true;
}

bool ResolveLocalTimeFromEpoch(int64_t epoch_second, const std::string& tz, std::tm* out) {
    if (!out) return false;

    const std::time_t epoch = static_cast<std::time_t>(epoch_second);
    if (IsUtcTimezone(tz)) {
        return gmtime_r(&epoch, out) != nullptr;
    }
    return FillLocalTime(GetThreadLocalCalendar(tz), epoch_second, out);
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
    if (IsUtcTimezone(tz)) return utc_epoch_second;

    int32_t offset_second = 0;
    if (!ResolveLocalOffsetSecond(GetThreadLocalCalendar(tz), utc_epoch_second, &offset_second)) {
        return utc_epoch_second;
    }
    return utc_epoch_second + static_cast<int64_t>(offset_second);
}

}  // namespace baseline
}  // namespace flowsql
