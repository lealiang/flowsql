/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_ROLLING_DETECTION_CALIBRATION_H_
#define _FLOWSQL_PLUGINS_BASELINE_ROLLING_DETECTION_CALIBRATION_H_

#include <framework/interfaces/ibaseline_types.h>

#include "plugins/baseline/rolling/observation_adapter.h"
#include "plugins/baseline/rolling/rolling_estimator.h"
#include "plugins/baseline/rolling/rolling_state.h"

namespace flowsql {
namespace baseline {

struct DetectionBandResult {
    BaselineStatus status = BaselineStatus::kOk;
    double detection_mu = 0.0;
    double model_lower = 0.0;
    double model_upper = 0.0;
    double detection_var = 0.0;
    double band_std = 0.0;
    double raw_detection_var = 0.0;
    double raw_band_std = 0.0;
    double raw_calibration_var = 0.0;
    double raw_z = 0.0;
    double pred_var_component = 0.0;
    double calibrated_sigma = 0.0;
    double calibrated_sigma_var = 0.0;
    double extra_obs_var = 0.0;
    double maturity_uncertainty_var = 0.0;
    double component_missing_uncertainty_var = 0.0;
    double residual = 0.0;
    double detection_z = 0.0;
    bool std_cap_applied = false;
    bool is_outside_band = false;
};

BaselineStatus BuildDetectionBand(const RollingState& state,
                                  const ObservedModelPoint& point,
                                  const RollingEstimatorResult& estimator,
                                  const BaselineRollingConfig& config,
                                  double active_monthpos_effect,
                                  DetectionBandResult* out);

BaselineStatus UpdateDetectionCalibration(const ObservedModelPoint& point,
                                          const DetectionBandResult& band,
                                          const BaselineRollingConfig& config,
                                          RollingState* state);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_ROLLING_DETECTION_CALIBRATION_H_
