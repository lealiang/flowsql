/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_FEATURE_BATCH_H_
#define _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_FEATURE_BATCH_H_

#include <framework/interfaces/ibaseline_types.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "plugins/baseline/model/calendar_feature_helper.h"
#include "plugins/baseline/rolling/rolling_config.h"

namespace flowsql {
namespace baseline {

struct RollingFeatureView {
    // RollingFeatureView 不拥有内存；父 RollingFeatureBatch 必须覆盖 view 的使用期。
    int64_t bucket_id = 0;
    const double* day_sin = nullptr;
    const double* day_cos = nullptr;
    std::size_t day_size = 0;
    const double* week_sin = nullptr;
    const double* week_cos = nullptr;
    std::size_t week_size = 0;
    const LocalCalendarFeature* calendar = nullptr;
    bool reanchored = false;
};

struct RollingFeatureBatch {
    int64_t start_bucket_id = 0;
    uint32_t point_count = 0;
    int32_t daily_order = 0;
    int32_t weekly_order = 0;
    std::vector<LocalCalendarFeature> calendar;
    std::vector<double> day_sin;
    std::vector<double> day_cos;
    std::vector<double> week_sin;
    std::vector<double> week_cos;
    std::vector<uint8_t> reanchored;

    RollingFeatureView View(uint32_t index) const;
};

uint32_t ComputeRollingFeatureChunkSize(int32_t daily_order, int32_t weekly_order);

BaselineStatus BuildRollingFeatureBatch(int64_t start_bucket_id,
                                        uint32_t point_count,
                                        const BaselineRollingConfig& config,
                                        RollingFeatureBatch* out);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_FEATURE_BATCH_H_
