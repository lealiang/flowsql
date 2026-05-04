/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <cassert>
#include <cmath>
#include <cstdint>
#include <ctime>

#include "plugins/baseline/rolling/rolling_config.h"
#include "plugins/baseline/rolling/rolling_estimator.h"
#include "plugins/baseline/rolling/rolling_feature_batch.h"

namespace flowsql {
namespace baseline {
namespace {

constexpr double kFeatureEps = 1.0e-11;

int64_t UtcBucket(int year,
                  int month,
                  int day,
                  int hour,
                  int minute,
                  int second,
                  int64_t bucket_seconds) {
    std::tm utc{};
    utc.tm_year = year - 1900;
    utc.tm_mon = month - 1;
    utc.tm_mday = day;
    utc.tm_hour = hour;
    utc.tm_min = minute;
    utc.tm_sec = second;
    const std::time_t epoch = timegm(&utc);
    assert(epoch >= 0);
    return static_cast<int64_t>(epoch) / bucket_seconds;
}

BaselineRollingConfig MakeFeatureConfig(const std::string& timezone,
                                        int64_t bucket_seconds,
                                        int32_t daily_order,
                                        int32_t weekly_order) {
    BaselineRollingConfig config;
    config.timezone = timezone;
    config.bucket_seconds = bucket_seconds;
    config.daily_harmonic_order = daily_order;
    config.weekly_harmonic_order = weekly_order;
    return config;
}

void AssertClose(double actual, double expected) {
    assert(std::fabs(actual - expected) <= kFeatureEps);
}

void TestBatchMatchesSinglePointFeatureVector() {
    BaselineRollingConfig config = MakeFeatureConfig("Asia/Shanghai", 300, 6, 3);
    const int64_t start_bucket = UtcBucket(2026, 5, 1, 0, 0, 0, config.bucket_seconds);

    RollingFeatureBatch batch;
    assert(BuildRollingFeatureBatch(start_bucket, 288, config, &batch) == BaselineStatus::kOk);
    assert(batch.point_count == 288);
    assert(batch.daily_order == 6);
    assert(batch.weekly_order == 3);

    for (uint32_t i = 0; i < batch.point_count; ++i) {
        const int64_t bucket_id = start_bucket + static_cast<int64_t>(i);
        RollingFeatureVector single;
        assert(BuildRollingFeatureVector(bucket_id, config, &single) == BaselineStatus::kOk);
        const RollingFeatureView view = batch.View(i);
        assert(view.bucket_id == bucket_id);
        assert(view.day_size == single.day_sin.size());
        assert(view.week_size == single.week_sin.size());
        assert(view.calendar != nullptr);
        assert(view.calendar->valid);

        for (std::size_t j = 0; j < view.day_size; ++j) {
            AssertClose(view.day_sin[j], single.day_sin[j]);
            AssertClose(view.day_cos[j], single.day_cos[j]);
        }
        for (std::size_t j = 0; j < view.week_size; ++j) {
            AssertClose(view.week_sin[j], single.week_sin[j]);
            AssertClose(view.week_cos[j], single.week_cos[j]);
        }
    }
}

void TestDstBoundaryReanchorsFourierRecurrence() {
    BaselineRollingConfig config = MakeFeatureConfig("America/New_York", 60, 6, 3);

    const int64_t spring_start = UtcBucket(2026, 3, 8, 6, 55, 0, config.bucket_seconds);
    RollingFeatureBatch spring;
    assert(BuildRollingFeatureBatch(spring_start, 10, config, &spring) == BaselineStatus::kOk);
    bool saw_spring_reanchor = false;
    for (uint32_t i = 1; i < spring.point_count; ++i) {
        const RollingFeatureView view = spring.View(i);
        const RollingFeatureView prev = spring.View(i - 1);
        if (view.calendar->local_wall_second - prev.calendar->local_wall_second !=
            config.bucket_seconds) {
            saw_spring_reanchor = view.reanchored;
        }
    }
    assert(saw_spring_reanchor);

    const int64_t fall_start = UtcBucket(2026, 11, 1, 5, 55, 0, config.bucket_seconds);
    RollingFeatureBatch fall;
    assert(BuildRollingFeatureBatch(fall_start, 10, config, &fall) == BaselineStatus::kOk);
    bool saw_fall_reanchor = false;
    for (uint32_t i = 1; i < fall.point_count; ++i) {
        const RollingFeatureView view = fall.View(i);
        const RollingFeatureView prev = fall.View(i - 1);
        if (view.calendar->local_wall_second - prev.calendar->local_wall_second !=
            config.bucket_seconds) {
            saw_fall_reanchor = view.reanchored;
        }
    }
    assert(saw_fall_reanchor);
}

void TestZeroHarmonicOrderStillBuildsCalendarBatch() {
    BaselineRollingConfig config = MakeFeatureConfig("UTC", 60, 0, 0);
    const int64_t start_bucket = UtcBucket(2026, 5, 1, 0, 0, 0, config.bucket_seconds);

    RollingFeatureBatch batch;
    assert(BuildRollingFeatureBatch(start_bucket, 4, config, &batch) == BaselineStatus::kOk);
    assert(batch.point_count == 4);
    for (uint32_t i = 0; i < batch.point_count; ++i) {
        const RollingFeatureView view = batch.View(i);
        assert(view.day_size == 0);
        assert(view.week_size == 0);
        assert(view.calendar != nullptr);
        assert(view.calendar->valid);
    }
}

}  // namespace
}  // namespace baseline
}  // namespace flowsql

int main() {
    flowsql::baseline::TestBatchMatchesSinglePointFeatureVector();
    flowsql::baseline::TestDstBoundaryReanchorsFourierRecurrence();
    flowsql::baseline::TestZeroHarmonicOrderStillBuildsCalendarBatch();
    return 0;
}
