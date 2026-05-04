/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_ESTIMATOR_H_
#define _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_ESTIMATOR_H_

#include <framework/interfaces/ibaseline_types.h>

#include <cstdint>
#include <vector>

#include "plugins/baseline/rolling/rolling_feature_batch.h"
#include "plugins/baseline/rolling/rolling_state.h"

namespace flowsql {
namespace baseline {

struct RollingFeatureVector {
    std::vector<double> day_sin;
    std::vector<double> day_cos;
    std::vector<double> week_sin;
    std::vector<double> week_cos;
};

struct RollingEstimatorResult {
    BaselineStatus status = BaselineStatus::kOk;
    int64_t bucket_id = 0;
    int64_t dt = 0;

    double model_mu = 0.0;
    double model_lower = 0.0;
    double model_upper = 0.0;
    double band_std = 0.0;
    double pred_var = 0.0;
    double obs_var = 0.0;
    double residual = 0.0;
    double z_score = 0.0;

    double pred_p_level = 0.0;
    bool did_update = false;
};

BaselineStatus BuildRollingFeatureVector(int64_t bucket_id,
                                         const BaselineRollingConfig& config,
                                         RollingFeatureVector* out);

BaselineStatus PredictRollingState(const RollingState& state,
                                   const ObservedModelPoint& point,
                                   const BaselineRollingConfig& config,
                                   RollingEstimatorResult* out);

BaselineStatus PredictRollingForecastState(const RollingState& state,
                                           int64_t bucket_id,
                                           const BaselineRollingConfig& config,
                                           RollingEstimatorResult* out);

BaselineStatus PredictRollingForecastStateWithFeature(const RollingState& state,
                                                      int64_t bucket_id,
                                                      const BaselineRollingConfig& config,
                                                      const RollingFeatureView& feature,
                                                      RollingEstimatorResult* out);

BaselineStatus UpdateRollingStateWithObservation(const ObservedModelPoint& point,
                                                 const BaselineRollingConfig& config,
                                                 RollingState* state,
                                                 RollingEstimatorResult* out,
                                                 double adapt_boost = 0.0);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_ESTIMATOR_H_
