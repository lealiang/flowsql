/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_CONFIG_H_
#define _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_CONFIG_H_

#include <framework/interfaces/ibaseline_types.h>

#include <cstdint>
#include <string>

#include "plugins/baseline/model/task_spec.h"

namespace flowsql {
namespace baseline {

struct BaselineRollingConfig {
    uint32_t n_min_score = 3;
    uint32_t n_min_update = 10;
    uint32_t n_ref = 10;
    double sample_count_noise = 1.0;

    uint32_t d_min_score = 10;
    uint32_t d_min_update = 100;
    uint32_t d_ref = 100;
    double ratio_denominator_noise = 1.0;

    double z_downweight = 3.0;
    double z_skip = 5.0;
    double small_update_weight = 0.2;

    int32_t daily_harmonic_order = 6;
    int32_t weekly_harmonic_order = 3;

    double level_learning_scale = 1.0;
    double day_learning_scale = 0.2;
    double week_learning_scale = 0.05;
    double cold_day_learning_scale = 0.05;
    double cold_week_learning_scale = 0.01;
    double seasonal_drift_min_scale = 0.1;

    double day_delta_coeff_max_scale = 0.05;
    double week_delta_coeff_max_scale = 0.02;

    double q_day_scale = 1.0e-4;
    double q_week_scale = 2.0e-5;
    double q_level_scale = 1.0e-3;
    double q_trend_scale = 1.0e-5;

    double trend_update_scale = 0.05;
    double cold_trend_update_scale = 0.0;
    double trend_delta_max_scale = 0.01;
    double trend_abs_max_scale = 0.1;

    double p_level_init_scale = 9.0;
    double p_trend_init_scale = 16.0;
    double p_day_init_scale = 4.0;
    double p_week_init_scale = 9.0;
    double p_floor_scale = 1.0e-6;
    double p_cap_scale = 1.0e6;

    double alpha_short = 0.05;
    double alpha_long = 0.005;
    double z_cap = 5.0;
    double drift_start = 1.0;
    double drift_full = 2.0;
    double max_level_boost = 4.0;
    double max_q_boost = 9.0;
    double skip_relax = 2.0;

    uint64_t process_noise_gap_cap_buckets = 0;

    double alpha_sigma = 0.02;
    double c_sigma = 3.0;
    double sigma_floor = 0.05;
    double cold_start_band_scale = 3.0;
    double band_z = 3.0;
    double confidence_cold = 0.2;
    double confidence_warming = 0.5;
    double confidence_ready_hint_cap = 0.8;
    uint64_t min_warming_updates = 3;
    uint64_t min_ready_hint_updates = 0;

    int64_t bucket_seconds = 60;
    std::string timezone = "UTC";
    uint64_t day_buckets = 1440;
    uint64_t week_buckets = 10080;
};

BaselineRollingConfig DefaultBaselineRollingConfig();

BaselineStatus ValidateBaselineRollingConfig(const BaselineRollingConfig& config,
                                             std::string* err = nullptr);

BaselineStatus __attribute__((visibility("default"))) ResolveBaselineRollingConfig(
    const BaselineTaskSpec& spec,
    BaselineRollingConfig* out,
    std::string* err = nullptr);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_CONFIG_H_
