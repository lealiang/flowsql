/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/rolling/rolling_estimator.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "plugins/baseline/model/calendar_feature_helper.h"

namespace flowsql {
namespace baseline {
namespace {

constexpr double kPi = 3.14159265358979323846;

double Clamp(double value, double lo, double hi) {
    return std::max(lo, std::min(hi, value));
}

double CovarianceFloor(const BaselineRollingConfig& config) {
    return config.p_floor_scale * config.sigma_floor * config.sigma_floor;
}

double CovarianceCap(const BaselineRollingConfig& config) {
    return config.p_cap_scale * config.sigma_floor * config.sigma_floor;
}

double ClampCovariance(double value, const BaselineRollingConfig& config) {
    return Clamp(value, CovarianceFloor(config), CovarianceCap(config));
}

double ProcessVariance(double q_scale,
                       double sigma,
                       int64_t dt,
                       const BaselineRollingConfig& config) {
    const uint64_t gap_cap =
        config.process_noise_gap_cap_buckets == 0
            ? std::max<uint64_t>(1, config.week_buckets)
            : config.process_noise_gap_cap_buckets;
    const uint64_t dt_q =
        std::min<uint64_t>(gap_cap, static_cast<uint64_t>(std::max<int64_t>(1, dt)));
    return q_scale * sigma * sigma * static_cast<double>(dt_q);
}

double SafeAdaptBoost(double adapt_boost) {
    if (!std::isfinite(adapt_boost)) return 0.0;
    return Clamp(adapt_boost, 0.0, 1.0);
}

double SeedSeasonalScale(RollingSeasonalPriorQuality quality,
                         double adapt_boost,
                         const BaselineRollingConfig& config) {
    if (quality == RollingSeasonalPriorQuality::kFull) {
        if (config.freeze_seeded_seasonal_on_drift && adapt_boost > 0.0) {
            return 0.0;
        }
        return config.full_seed_seasonal_scale;
    }
    if (quality == RollingSeasonalPriorQuality::kPartial) {
        return config.partial_seed_seasonal_scale;
    }
    return 1.0;
}

double EvaluateHarmonic(const RollingHarmonicState& harmonic,
                        const std::vector<double>& sin_feature,
                        const std::vector<double>& cos_feature) {
    double value = 0.0;
    const std::size_t size =
        std::min(harmonic.sin_coeff.size(), sin_feature.size());
    for (std::size_t i = 0; i < size; ++i) {
        value += harmonic.sin_coeff[i] * sin_feature[i];
    }
    const std::size_t cos_size =
        std::min(harmonic.cos_coeff.size(), cos_feature.size());
    for (std::size_t i = 0; i < cos_size; ++i) {
        value += harmonic.cos_coeff[i] * cos_feature[i];
    }
    return value;
}

double HarmonicVariance(const RollingHarmonicState& harmonic,
                        const std::vector<double>& sin_feature,
                        const std::vector<double>& cos_feature) {
    double value = 0.0;
    const std::size_t size = std::min(harmonic.sin_p.size(), sin_feature.size());
    for (std::size_t i = 0; i < size; ++i) {
        value += harmonic.sin_p[i] * sin_feature[i] * sin_feature[i];
    }
    const std::size_t cos_size = std::min(harmonic.cos_p.size(), cos_feature.size());
    for (std::size_t i = 0; i < cos_size; ++i) {
        value += harmonic.cos_p[i] * cos_feature[i] * cos_feature[i];
    }
    return value;
}

void AddProcessNoiseToHarmonic(double q,
                               const BaselineRollingConfig& config,
                               RollingHarmonicState* harmonic) {
    for (double& p : harmonic->sin_p) p = ClampCovariance(p + q, config);
    for (double& p : harmonic->cos_p) p = ClampCovariance(p + q, config);
}

void SymmetricClampLevelTrendCovariance(const BaselineRollingConfig& config,
                                        RollingState* state) {
    state->p_level = ClampCovariance(state->p_level, config);
    state->p_trend = ClampCovariance(state->p_trend, config);
    const double cross_cap = std::sqrt(state->p_level * state->p_trend);
    state->p_level_trend = Clamp(state->p_level_trend, -cross_cap, cross_cap);
}

RollingState AdvancedState(const RollingState& state,
                           int64_t dt,
                           const BaselineRollingConfig& config,
                           double adapt_boost = 0.0) {
    RollingState advanced = state;
    const double dt_d = static_cast<double>(dt);
    const double safe_boost = SafeAdaptBoost(adapt_boost);
    const double q_level_scale =
        config.q_level_scale * (1.0 + safe_boost * config.max_q_boost);
    const double q_level = ProcessVariance(q_level_scale, state.sigma, dt, config);
    const double q_trend = ProcessVariance(config.q_trend_scale, state.sigma, dt, config);
    const double q_day = ProcessVariance(config.q_day_scale, state.sigma, dt, config);
    const double q_week = ProcessVariance(config.q_week_scale, state.sigma, dt, config);

    const double p_level = state.p_level;
    const double p_level_trend = state.p_level_trend;
    const double p_trend = state.p_trend;

    advanced.theta.level = state.theta.level + state.theta.trend * dt_d;
    advanced.p_level =
        p_level + 2.0 * dt_d * p_level_trend + dt_d * dt_d * p_trend + q_level;
    advanced.p_level_trend = p_level_trend + dt_d * p_trend;
    advanced.p_trend = p_trend + q_trend;
    SymmetricClampLevelTrendCovariance(config, &advanced);

    AddProcessNoiseToHarmonic(q_day, config, &advanced.theta.daily);
    AddProcessNoiseToHarmonic(q_week, config, &advanced.theta.weekly);
    return advanced;
}

double ExtraObsVariance(const ObservedModelPoint& point,
                        const RollingState& state,
                        const BaselineRollingConfig& config) {
    return std::max(0.0, point.extra_obs_noise_scale) * state.sigma * state.sigma +
           state.sigma * state.sigma + config.sigma_floor * config.sigma_floor;
}

double ClipDelta(double value, double cap) {
    return Clamp(value, -cap, cap);
}

double ForecastTrendSteps(int64_t dt, const BaselineRollingConfig& config) {
    const uint64_t cap =
        config.forecast_trend_cap_buckets == 0
            ? std::max<uint64_t>(1, config.day_buckets)
            : config.forecast_trend_cap_buckets;
    return static_cast<double>(
        std::min<uint64_t>(cap, static_cast<uint64_t>(std::max<int64_t>(1, dt))));
}

void UpdateDiagonalHarmonic(const std::vector<double>& feature,
                            double residual,
                            double s,
                            double r,
                            double weight,
                            double delta_cap,
                            const BaselineRollingConfig& config,
                            std::vector<double>* coeff,
                            std::vector<double>* covariance) {
    const std::size_t size = std::min(coeff->size(), std::min(covariance->size(), feature.size()));
    for (std::size_t i = 0; i < size; ++i) {
        const double h = feature[i];
        const double p = (*covariance)[i];
        const double k = p * h / s;
        const double delta = ClipDelta(weight * k * residual, delta_cap);
        (*coeff)[i] += delta;
        const double a = 1.0 - weight * k * h;
        (*covariance)[i] =
            ClampCovariance(a * a * p + (weight * k) * (weight * k) * r, config);
    }
}

BaselineStatus FillPrediction(const RollingState& advanced,
                              const ObservedModelPoint& point,
                              const BaselineRollingConfig& config,
                              const RollingFeatureVector& feature,
                              int64_t dt,
                              RollingEstimatorResult* out) {
    if (!out) return BaselineStatus::kInvalidArgument;

    RollingEstimatorResult result;
    result.status = BaselineStatus::kOk;
    result.bucket_id = point.bucket_id;
    result.dt = dt;
    result.model_mu =
        advanced.theta.level +
        EvaluateHarmonic(advanced.theta.daily, feature.day_sin, feature.day_cos) +
        EvaluateHarmonic(advanced.theta.weekly, feature.week_sin, feature.week_cos);
    result.pred_var =
        advanced.p_level +
        HarmonicVariance(advanced.theta.daily, feature.day_sin, feature.day_cos) +
        HarmonicVariance(advanced.theta.weekly, feature.week_sin, feature.week_cos);
    const double obs_var = result.pred_var + ExtraObsVariance(point, advanced, config);
    if (!std::isfinite(obs_var) || obs_var <= 0.0) {
        return BaselineStatus::kInvalidArgument;
    }
    result.obs_var = obs_var;
    result.band_std = std::sqrt(obs_var);
    result.model_lower = result.model_mu - config.band_z * result.band_std;
    result.model_upper = result.model_mu + config.band_z * result.band_std;
    result.residual = point.y_model - result.model_mu;
    result.z_score = point.can_score ? std::fabs(result.residual / result.band_std) : 0.0;
    result.pred_p_level = advanced.p_level;
    *out = result;
    return BaselineStatus::kOk;
}

}  // namespace

BaselineStatus BuildRollingFeatureVector(int64_t bucket_id,
                                         const BaselineRollingConfig& config,
                                         RollingFeatureVector* out) {
    if (!out || config.bucket_seconds <= 0) return BaselineStatus::kInvalidArgument;

    RollingFeatureVector feature;
    const double day_phase = PhaseDayLocal(bucket_id, config.bucket_seconds, config.timezone);
    const double week_phase = PhaseWeekLocal(bucket_id, config.bucket_seconds, config.timezone);

    const std::size_t day_size = static_cast<std::size_t>(std::max(0, config.daily_harmonic_order));
    const std::size_t week_size =
        static_cast<std::size_t>(std::max(0, config.weekly_harmonic_order));
    feature.day_sin.resize(day_size, 0.0);
    feature.day_cos.resize(day_size, 0.0);
    feature.week_sin.resize(week_size, 0.0);
    feature.week_cos.resize(week_size, 0.0);

    for (std::size_t i = 0; i < day_size; ++i) {
        const double angle = 2.0 * kPi * static_cast<double>(i + 1) * day_phase;
        feature.day_sin[i] = std::sin(angle);
        feature.day_cos[i] = std::cos(angle);
    }
    for (std::size_t i = 0; i < week_size; ++i) {
        const double angle = 2.0 * kPi * static_cast<double>(i + 1) * week_phase;
        feature.week_sin[i] = std::sin(angle);
        feature.week_cos[i] = std::cos(angle);
    }

    *out = std::move(feature);
    return BaselineStatus::kOk;
}

BaselineStatus PredictRollingState(const RollingState& state,
                                   const ObservedModelPoint& point,
                                   const BaselineRollingConfig& config,
                                   RollingEstimatorResult* out) {
    if (!out || !state.has_seen_observation || point.series_key != state.series_key ||
        point.status != BaselineStatus::kOk || !std::isfinite(point.y_model)) {
        return BaselineStatus::kInvalidArgument;
    }
    const int64_t dt = point.bucket_id - state.last_seen_bucket;
    if (dt <= 0) return BaselineStatus::kInvalidArgument;

    RollingFeatureVector feature;
    const BaselineStatus feature_status =
        BuildRollingFeatureVector(point.bucket_id, config, &feature);
    if (feature_status != BaselineStatus::kOk) return feature_status;

    const RollingState advanced = AdvancedState(state, dt, config);
    return FillPrediction(advanced, point, config, feature, dt, out);
}

BaselineStatus PredictRollingForecastState(const RollingState& state,
                                           int64_t bucket_id,
                                           const BaselineRollingConfig& config,
                                           RollingEstimatorResult* out) {
    if (!out || !state.has_seen_observation) {
        return BaselineStatus::kInvalidArgument;
    }
    const int64_t dt = bucket_id - state.last_seen_bucket;
    if (dt <= 0) return BaselineStatus::kInvalidArgument;

    RollingFeatureVector feature;
    const BaselineStatus feature_status =
        BuildRollingFeatureVector(bucket_id, config, &feature);
    if (feature_status != BaselineStatus::kOk) return feature_status;

    const double trend_steps = ForecastTrendSteps(dt, config);
    const double seasonal_mu =
        EvaluateHarmonic(state.theta.daily, feature.day_sin, feature.day_cos) +
        EvaluateHarmonic(state.theta.weekly, feature.week_sin, feature.week_cos);
    // Forecast view is a conditional baseline curve. The local trend may move
    // the center line, but its covariance is not treated as long-horizon drift
    // risk; drift risk is handled by online detection trust.
    const double raw_pred_var =
        state.p_level +
        HarmonicVariance(state.theta.daily, feature.day_sin, feature.day_cos) +
        HarmonicVariance(state.theta.weekly, feature.week_sin, feature.week_cos);
    const double pred_var = std::max(0.0, raw_pred_var);
    const double sigma =
        std::max(config.sigma_floor,
                 std::isfinite(state.sigma) ? state.sigma : config.sigma_floor);
    const double obs_var = pred_var + sigma * sigma + config.sigma_floor * config.sigma_floor;
    if (!std::isfinite(obs_var) || obs_var <= 0.0) {
        return BaselineStatus::kInvalidArgument;
    }

    RollingEstimatorResult result;
    result.status = BaselineStatus::kOk;
    result.bucket_id = bucket_id;
    result.dt = dt;
    result.model_mu = state.theta.level + state.theta.trend * trend_steps + seasonal_mu;
    result.pred_var = pred_var;
    result.obs_var = obs_var;
    result.band_std = std::sqrt(obs_var);
    result.model_lower = result.model_mu - config.band_z * result.band_std;
    result.model_upper = result.model_mu + config.band_z * result.band_std;
    result.residual = 0.0;
    result.z_score = 0.0;
    result.pred_p_level = pred_var;
    *out = result;
    return BaselineStatus::kOk;
}

BaselineStatus UpdateRollingStateWithObservation(const ObservedModelPoint& point,
                                                 const BaselineRollingConfig& config,
                                                 RollingState* state,
                                                 RollingEstimatorResult* out,
                                                 double adapt_boost) {
    if (!state || !out) return BaselineStatus::kInvalidArgument;
    if (!state->has_seen_observation || point.series_key != state->series_key ||
        point.status != BaselineStatus::kOk || !std::isfinite(point.y_model)) {
        return BaselineStatus::kInvalidArgument;
    }
    const int64_t dt = point.bucket_id - state->last_seen_bucket;
    if (dt <= 0) return BaselineStatus::kInvalidArgument;

    RollingFeatureVector feature;
    const BaselineStatus feature_status =
        BuildRollingFeatureVector(point.bucket_id, config, &feature);
    if (feature_status != BaselineStatus::kOk) return feature_status;

    const double safe_boost = SafeAdaptBoost(adapt_boost);
    RollingState advanced = AdvancedState(*state, dt, config, safe_boost);
    RollingEstimatorResult result;
    BaselineStatus status = FillPrediction(advanced, point, config, feature, dt, &result);
    if (status != BaselineStatus::kOk) return status;

    if (!point.can_update || point.update_weight <= 0.0) {
        *out = result;
        return BaselineStatus::kOk;
    }

    const double s = result.obs_var;
    const double r = result.obs_var - result.pred_var;
    if (!std::isfinite(r) || r <= 0.0) return BaselineStatus::kInvalidArgument;
    const double residual = result.residual;
    const double base_update_weight = Clamp(point.update_weight, 0.0, 1.0);
    const bool cold = advanced.state_status == RollingStateStatus::kColdLearning;
    const double seasonal_drift_scale =
        std::max(config.seasonal_drift_min_scale, 1.0 - safe_boost);
    const double day_seed_scale =
        SeedSeasonalScale(advanced.daily_prior_quality, safe_boost, config);
    const double week_seed_scale =
        SeedSeasonalScale(advanced.weekly_prior_quality, safe_boost, config);
    const double level_boost = 1.0 + safe_boost * config.max_level_boost;
    const double w_level =
        Clamp(base_update_weight * config.level_learning_scale * level_boost, 0.0, 1.0);
    const double w_trend =
        cold ? std::min(w_level,
                        Clamp(base_update_weight * config.cold_trend_update_scale, 0.0, 1.0))
             : std::min(w_level,
                        Clamp(base_update_weight * config.trend_update_scale, 0.0, 1.0));
    const double w_day =
        Clamp(base_update_weight *
                  (cold ? config.cold_day_learning_scale : config.day_learning_scale) *
                  seasonal_drift_scale * day_seed_scale,
              0.0,
              1.0);
    const double w_week =
        Clamp(base_update_weight *
                  (cold ? config.cold_week_learning_scale : config.week_learning_scale) *
                  seasonal_drift_scale * week_seed_scale,
              0.0,
              1.0);

    const double k_level = advanced.p_level / s;
    const double k_trend = advanced.p_level_trend / s;
    const double trend_cap = config.trend_delta_max_scale * advanced.sigma;
    const double day_cap = config.day_delta_coeff_max_scale * advanced.sigma;
    const double week_cap = config.week_delta_coeff_max_scale * advanced.sigma;

    advanced.theta.level += w_level * k_level * residual;
    advanced.theta.trend += ClipDelta(w_trend * k_trend * residual, trend_cap);
    advanced.theta.trend =
        Clamp(advanced.theta.trend,
              -config.trend_abs_max_scale * advanced.sigma,
              config.trend_abs_max_scale * advanced.sigma);

    const double a00 = 1.0 - w_level * k_level;
    const double a01 = 0.0;
    const double a10 = -w_trend * k_trend;
    const double a11 = 1.0;
    const double p00 = advanced.p_level;
    const double p01 = advanced.p_level_trend;
    const double p11 = advanced.p_trend;
    const double ap00 = a00 * p00 + a01 * p01;
    const double ap01 = a00 * p01 + a01 * p11;
    const double ap10 = a10 * p00 + a11 * p01;
    const double ap11 = a10 * p01 + a11 * p11;
    advanced.p_level = ap00 * a00 + ap01 * a01 +
                       (w_level * k_level) * (w_level * k_level) * r;
    advanced.p_level_trend = ap10 * a00 + ap11 * a01 +
                             (w_trend * k_trend) * (w_level * k_level) * r;
    advanced.p_trend = ap10 * a10 + ap11 * a11 +
                       (w_trend * k_trend) * (w_trend * k_trend) * r;
    SymmetricClampLevelTrendCovariance(config, &advanced);

    UpdateDiagonalHarmonic(feature.day_sin,
                           residual,
                           s,
                           r,
                           w_day,
                           day_cap,
                           config,
                           &advanced.theta.daily.sin_coeff,
                           &advanced.theta.daily.sin_p);
    UpdateDiagonalHarmonic(feature.day_cos,
                           residual,
                           s,
                           r,
                           w_day,
                           day_cap,
                           config,
                           &advanced.theta.daily.cos_coeff,
                           &advanced.theta.daily.cos_p);
    UpdateDiagonalHarmonic(feature.week_sin,
                           residual,
                           s,
                           r,
                           w_week,
                           week_cap,
                           config,
                           &advanced.theta.weekly.sin_coeff,
                           &advanced.theta.weekly.sin_p);
    UpdateDiagonalHarmonic(feature.week_cos,
                           residual,
                           s,
                           r,
                           w_week,
                           week_cap,
                           config,
                           &advanced.theta.weekly.cos_coeff,
                           &advanced.theta.weekly.cos_p);

    advanced.last_seen_bucket = point.bucket_id;
    advanced.accepted_update_count += 1;
    advanced.state_status =
        StatusFromAcceptedUpdateCount(advanced.accepted_update_count, config);
    result.did_update = true;
    *state = std::move(advanced);
    *out = result;
    return BaselineStatus::kOk;
}

}  // namespace baseline
}  // namespace flowsql
