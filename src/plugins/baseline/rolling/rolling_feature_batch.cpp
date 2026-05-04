/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/rolling/rolling_feature_batch.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace flowsql {
namespace baseline {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kMaxBatchFeatureChunkSize = 4096;
constexpr uint32_t kMinBatchFeatureChunkSize = 256;
constexpr uint64_t kTargetBatchFourierBytes = 2ULL * 1024ULL * 1024ULL;
constexpr uint32_t kFourierReanchorInterval = 1024;
constexpr double kSecondsPerDay = 24.0 * 60.0 * 60.0;
constexpr double kSecondsPerWeek = 7.0 * kSecondsPerDay;

std::size_t FeatureOffset(uint32_t index, int32_t order) {
    return static_cast<std::size_t>(index) * static_cast<std::size_t>(order);
}

void FillDirect(double phase,
                int32_t order,
                double* sin_out,
                double* cos_out) {
    for (int32_t i = 0; i < order; ++i) {
        const double angle = 2.0 * kPi * static_cast<double>(i + 1) * phase;
        sin_out[i] = std::sin(angle);
        cos_out[i] = std::cos(angle);
    }
}

void FillStep(int32_t order,
              double denominator,
              int64_t bucket_seconds,
              std::vector<double>* sin_step,
              std::vector<double>* cos_step) {
    sin_step->assign(static_cast<std::size_t>(order), 0.0);
    cos_step->assign(static_cast<std::size_t>(order), 1.0);
    for (int32_t i = 0; i < order; ++i) {
        const double angle =
            2.0 * kPi * static_cast<double>(i + 1) *
            static_cast<double>(bucket_seconds) / denominator;
        (*sin_step)[static_cast<std::size_t>(i)] = std::sin(angle);
        (*cos_step)[static_cast<std::size_t>(i)] = std::cos(angle);
    }
}

void FillRecurrence(uint32_t index,
                    int32_t order,
                    const std::vector<double>& sin_step,
                    const std::vector<double>& cos_step,
                    std::vector<double>* sin_values,
                    std::vector<double>* cos_values) {
    const std::size_t prev_offset = FeatureOffset(index - 1, order);
    const std::size_t offset = FeatureOffset(index, order);
    for (int32_t i = 0; i < order; ++i) {
        const std::size_t j = static_cast<std::size_t>(i);
        const double prev_sin = (*sin_values)[prev_offset + j];
        const double prev_cos = (*cos_values)[prev_offset + j];
        const double step_sin = sin_step[j];
        const double step_cos = cos_step[j];
        (*sin_values)[offset + j] = prev_sin * step_cos + prev_cos * step_sin;
        (*cos_values)[offset + j] = prev_cos * step_cos - prev_sin * step_sin;
    }
}

bool ShouldReanchor(const RollingFeatureBatch& batch,
                    uint32_t index,
                    int64_t bucket_seconds,
                    uint32_t steps_since_anchor) {
    if (index == 0) return true;
    const LocalCalendarFeature& current = batch.calendar[index];
    const LocalCalendarFeature& previous = batch.calendar[index - 1];
    if (!current.valid || !previous.valid) return true;
    if (current.bucket_id != previous.bucket_id + 1) return true;
    if (current.local_wall_second - previous.local_wall_second != bucket_seconds) {
        return true;
    }
    return steps_since_anchor >= kFourierReanchorInterval;
}

}  // namespace

RollingFeatureView RollingFeatureBatch::View(uint32_t index) const {
    RollingFeatureView view;
    if (index >= point_count) return view;
    view.bucket_id = start_bucket_id + static_cast<int64_t>(index);
    view.day_size = static_cast<std::size_t>(daily_order);
    view.week_size = static_cast<std::size_t>(weekly_order);
    view.calendar = index < calendar.size() ? &calendar[index] : nullptr;
    view.reanchored = index < reanchored.size() && reanchored[index] != 0;
    if (daily_order > 0) {
        const std::size_t offset = FeatureOffset(index, daily_order);
        view.day_sin = day_sin.data() + offset;
        view.day_cos = day_cos.data() + offset;
    }
    if (weekly_order > 0) {
        const std::size_t offset = FeatureOffset(index, weekly_order);
        view.week_sin = week_sin.data() + offset;
        view.week_cos = week_cos.data() + offset;
    }
    return view;
}

uint32_t ComputeRollingFeatureChunkSize(int32_t daily_order, int32_t weekly_order) {
    const uint64_t day = static_cast<uint64_t>(std::max(0, daily_order));
    const uint64_t week = static_cast<uint64_t>(std::max(0, weekly_order));
    const uint64_t per_point_doubles = 2ULL * (day + week);
    if (per_point_doubles == 0) return kMaxBatchFeatureChunkSize;
    const uint64_t by_memory =
        kTargetBatchFourierBytes / (per_point_doubles * sizeof(double));
    const uint64_t clamped =
        std::min<uint64_t>(
            kMaxBatchFeatureChunkSize,
            std::max<uint64_t>(kMinBatchFeatureChunkSize, by_memory));
    return static_cast<uint32_t>(clamped);
}

BaselineStatus BuildRollingFeatureBatch(int64_t start_bucket_id,
                                        uint32_t point_count,
                                        const BaselineRollingConfig& config,
                                        RollingFeatureBatch* out) {
    if (!out || point_count == 0 || config.bucket_seconds <= 0) {
        return BaselineStatus::kInvalidArgument;
    }
    if (start_bucket_id > std::numeric_limits<int64_t>::max() -
                              static_cast<int64_t>(point_count - 1)) {
        return BaselineStatus::kInvalidArgument;
    }

    RollingFeatureBatch batch;
    batch.start_bucket_id = start_bucket_id;
    batch.point_count = point_count;
    batch.daily_order = std::max(0, config.daily_harmonic_order);
    batch.weekly_order = std::max(0, config.weekly_harmonic_order);
    batch.calendar.resize(point_count);
    batch.reanchored.assign(point_count, 0);
    batch.day_sin.assign(
        static_cast<std::size_t>(point_count) * static_cast<std::size_t>(batch.daily_order),
        0.0);
    batch.day_cos.assign(
        static_cast<std::size_t>(point_count) * static_cast<std::size_t>(batch.daily_order),
        0.0);
    batch.week_sin.assign(
        static_cast<std::size_t>(point_count) * static_cast<std::size_t>(batch.weekly_order),
        0.0);
    batch.week_cos.assign(
        static_cast<std::size_t>(point_count) * static_cast<std::size_t>(batch.weekly_order),
        0.0);

    std::vector<double> daily_step_sin;
    std::vector<double> daily_step_cos;
    std::vector<double> weekly_step_sin;
    std::vector<double> weekly_step_cos;
    FillStep(batch.daily_order,
             kSecondsPerDay,
             config.bucket_seconds,
             &daily_step_sin,
             &daily_step_cos);
    FillStep(batch.weekly_order,
             kSecondsPerWeek,
             config.bucket_seconds,
             &weekly_step_sin,
             &weekly_step_cos);

    uint32_t steps_since_anchor = 0;
    for (uint32_t i = 0; i < point_count; ++i) {
        const int64_t bucket_id = start_bucket_id + static_cast<int64_t>(i);
        if (!ResolveOneLocalCalendarFeature(
                bucket_id, config.bucket_seconds, config.timezone, &batch.calendar[i])) {
            batch.calendar[i] = LocalCalendarFeature{};
            batch.calendar[i].bucket_id = bucket_id;
            batch.calendar[i].epoch_second = bucket_id * config.bucket_seconds;
        }

        const bool reanchor =
            ShouldReanchor(batch, i, config.bucket_seconds, steps_since_anchor);
        batch.reanchored[i] = reanchor ? 1 : 0;
        if (reanchor) {
            if (batch.daily_order > 0) {
                const std::size_t offset = FeatureOffset(i, batch.daily_order);
                FillDirect(batch.calendar[i].day_phase,
                           batch.daily_order,
                           batch.day_sin.data() + offset,
                           batch.day_cos.data() + offset);
            }
            if (batch.weekly_order > 0) {
                const std::size_t offset = FeatureOffset(i, batch.weekly_order);
                FillDirect(batch.calendar[i].week_phase,
                           batch.weekly_order,
                           batch.week_sin.data() + offset,
                           batch.week_cos.data() + offset);
            }
            steps_since_anchor = 0;
        } else {
            if (batch.daily_order > 0) {
                FillRecurrence(
                    i, batch.daily_order, daily_step_sin, daily_step_cos, &batch.day_sin, &batch.day_cos);
            }
            if (batch.weekly_order > 0) {
                FillRecurrence(i,
                               batch.weekly_order,
                               weekly_step_sin,
                               weekly_step_cos,
                               &batch.week_sin,
                               &batch.week_cos);
            }
            ++steps_since_anchor;
        }
    }

    *out = std::move(batch);
    return BaselineStatus::kOk;
}

}  // namespace baseline
}  // namespace flowsql
