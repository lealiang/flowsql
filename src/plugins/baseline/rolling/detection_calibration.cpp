/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/rolling/detection_calibration.h"

#include <algorithm>
#include <cmath>

#include "plugins/baseline/rolling/math_utils.h"

namespace flowsql {
namespace baseline {
namespace {

double SafeSigma(const RollingState& state, const BaselineRollingConfig& config) {
    return std::max(config.sigma_floor, std::isfinite(state.sigma) ? state.sigma
                                                                   : config.sigma_floor);
}

double MaturityUncertainty(const RollingState& state, const BaselineRollingConfig& config) {
    const double sigma2 = Square(SafeSigma(state, config));
    if (state.score_trust_status == ScoreTrustStatus::kDriftLearning) {
        return config.maturity_uncertainty_drift_scale * sigma2;
    }
    if (state.score_trust_status == ScoreTrustStatus::kRecalibrating) {
        return config.maturity_uncertainty_recalibrating_scale * sigma2;
    }
    if (state.maturity_status == RollingMaturityStatus::kColdLearning) {
        return config.maturity_uncertainty_cold_scale * sigma2;
    }
    if (state.score_trust_status == ScoreTrustStatus::kScoreWarming ||
        state.score_trust_status == ScoreTrustStatus::kScoreUntrusted) {
        return config.maturity_uncertainty_warming_scale * sigma2;
    }
    return 0.0;
}

double ComponentMissingUncertainty(const RollingState& state,
                                   const BaselineRollingConfig& config) {
    const double sigma2 = Square(SafeSigma(state, config));
    double value = 0.0;
    if (!MaturityAtLeast(state.maturity_status, RollingMaturityStatus::kDailyReady)) {
        value += config.missing_daily_uncertainty_scale * sigma2;
    }
    if (!MaturityAtLeast(state.maturity_status, RollingMaturityStatus::kWeeklyReady)) {
        value += config.missing_weekly_uncertainty_scale * sigma2;
    }
    return value;
}

}  // namespace

BaselineStatus BuildDetectionBand(const RollingState& state,
                                  const ObservedModelPoint& point,
                                  const RollingEstimatorResult& estimator,
                                  const BaselineRollingConfig& config,
                                  double active_monthpos_effect,
                                  DetectionBandResult* out) {
    if (!out || estimator.status != BaselineStatus::kOk ||
        point.status != BaselineStatus::kOk) {
        return BaselineStatus::kInvalidArgument;
    }
    const double sigma = SafeSigma(state, config);
    const double multiplier = ClampMultiplier(state.detection_band_multiplier, config);
    const double calibrated_sigma = std::max(config.sigma_floor, sigma * multiplier);
    const double pred_var_component = std::max(0.0, estimator.pred_var);
    const double extra_obs_var =
        std::max(0.0, point.extra_obs_noise_scale) * sigma * sigma +
        config.sigma_floor * config.sigma_floor;
    const double raw_calibration_var = pred_var_component + sigma * sigma + extra_obs_var;
    const double calibrated_sigma_var = calibrated_sigma * calibrated_sigma;
    const double maturity_uncertainty_var = MaturityUncertainty(state, config);
    const double component_missing_uncertainty_var =
        ComponentMissingUncertainty(state, config);
    double detection_var =
        pred_var_component +
        calibrated_sigma_var +
        extra_obs_var;
    if (!std::isfinite(detection_var) || detection_var <= 0.0) {
        return BaselineStatus::kInvalidArgument;
    }
    if (!std::isfinite(raw_calibration_var) || raw_calibration_var <= 0.0) {
        return BaselineStatus::kInvalidArgument;
    }

    DetectionBandResult result;
    result.status = BaselineStatus::kOk;
    result.detection_mu = estimator.model_mu + active_monthpos_effect;
    result.residual = point.y_model - result.detection_mu;
    result.raw_detection_var = detection_var;
    result.raw_band_std = std::sqrt(detection_var);
    result.raw_calibration_var = raw_calibration_var;
    result.raw_z =
        point.can_score ? std::fabs(result.residual / std::sqrt(raw_calibration_var)) : 0.0;
    result.pred_var_component = pred_var_component;
    result.calibrated_sigma = calibrated_sigma;
    result.calibrated_sigma_var = calibrated_sigma_var;
    result.extra_obs_var = extra_obs_var;
    result.maturity_uncertainty_var = maturity_uncertainty_var;
    result.component_missing_uncertainty_var = component_missing_uncertainty_var;
    result.band_std = std::min(result.raw_band_std, config.detection_band_std_cap);
    result.std_cap_applied = result.band_std < result.raw_band_std;
    detection_var = result.band_std * result.band_std;
    result.detection_var = detection_var;
    result.model_lower = result.detection_mu - config.band_z * result.band_std;
    result.model_upper = result.detection_mu + config.band_z * result.band_std;
    result.detection_z =
        point.can_score ? std::fabs(result.residual / result.band_std) : 0.0;
    result.is_outside_band =
        point.can_score &&
        (point.y_model < result.model_lower || point.y_model > result.model_upper);
    *out = result;
    return BaselineStatus::kOk;
}

BaselineStatus UpdateDetectionCalibration(const ObservedModelPoint& point,
                                          const DetectionBandResult& band,
                                          const BaselineRollingConfig& config,
                                          RollingState* state) {
    if (!state || point.status != BaselineStatus::kOk ||
        band.status != BaselineStatus::kOk || !std::isfinite(band.detection_z) ||
        !std::isfinite(band.raw_z)) {
        return BaselineStatus::kInvalidArgument;
    }
    if (!point.can_score || !point.can_update) return BaselineStatus::kOk;

    const double alpha = config.calibration_alpha;
    const double inside = band.detection_z <= config.band_z ? 1.0 : 0.0;
    const double tail3 = band.detection_z > 3.0 ? 1.0 : 0.0;
    const double tail5 = band.detection_z > 5.0 ? 1.0 : 0.0;
    state->coverage_ewma = (1.0 - alpha) * state->coverage_ewma + alpha * inside;
    state->tail3_ewma = (1.0 - alpha) * state->tail3_ewma + alpha * tail3;
    state->tail5_ewma = (1.0 - alpha) * state->tail5_ewma + alpha * tail5;
    state->abs_z_ewma =
        (1.0 - alpha) * state->abs_z_ewma +
        alpha * std::min(std::fabs(band.detection_z), config.z_cap);
    const double clipped_raw_z = std::min(std::fabs(band.raw_z), config.z_cap);
    const double previous_multiplier = state->detection_band_multiplier;
    if (!std::isfinite(state->residual_scale_ewma) || state->residual_scale_ewma <= 0.0) {
        state->residual_scale_ewma = 1.0;
    }
    state->residual_scale_ewma =
        (1.0 - alpha) * state->residual_scale_ewma +
        alpha * clipped_raw_z * clipped_raw_z;
    state->calibration_update_count += 1;

    if (state->calibration_update_count < config.calibration_warmup_min_updates) {
        state->calibration_status = RollingCalibrationStatus::kWarming;
        return BaselineStatus::kOk;
    }

    state->detection_band_multiplier =
        ClampMultiplier(std::sqrt(std::max(0.0, state->residual_scale_ewma)), config);
    if (state->detection_band_multiplier > previous_multiplier + 1.0e-12) {
        state->calibration_status = RollingCalibrationStatus::kExpanding;
        return BaselineStatus::kOk;
    }

    state->calibration_status = RollingCalibrationStatus::kCalibrated;
    return BaselineStatus::kOk;
}

}  // namespace baseline
}  // namespace flowsql
