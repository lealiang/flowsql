/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/rolling/rolling_task_runner.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "plugins/baseline/rolling/detection_calibration.h"
#include "plugins/baseline/rolling/drift_adapt.h"
#include "plugins/baseline/rolling/maturity_gate.h"
#include "plugins/baseline/rolling/monthpos_state.h"
#include "plugins/baseline/rolling/observation_adapter.h"
#include "plugins/baseline/rolling/residual_scale.h"
#include "plugins/baseline/rolling/rolling_config.h"
#include "plugins/baseline/rolling/rolling_estimator.h"
#include "plugins/baseline/rolling/score_trust.h"
#include "plugins/baseline/rolling/update_gate.h"

namespace flowsql {
namespace baseline {
namespace {

double Sigmoid(double value) {
    if (value >= 0.0) {
        const double e = std::exp(-value);
        return 1.0 / (1.0 + e);
    }
    const double e = std::exp(value);
    return e / (1.0 + e);
}

double ModelToObservedLower(const BaselineTaskSpec& spec, double value) {
    if (spec.feature_type == "ratio") return Sigmoid(value);
    return std::max(0.0, std::expm1(value));
}

double ModelToObservedUpper(const BaselineTaskSpec& spec, double value) {
    if (spec.feature_type == "ratio") return Sigmoid(value);
    return std::max(0.0, std::expm1(value));
}

double ModelToObservedMu(const BaselineTaskSpec& spec, double value) {
    if (spec.feature_type == "ratio") return Sigmoid(value);
    return std::max(0.0, std::expm1(value));
}

std::string BuildBandDiagnostics(const DetectionBandResult& band,
                                 const RollingState& state,
                                 const BaselineRollingConfig& config) {
    std::ostringstream out;
    const double combined_drift_evidence =
        std::fabs(state.drift_evidence) >= std::fabs(state.level_shift_evidence)
            ? state.drift_evidence
            : state.level_shift_evidence;
    out << std::setprecision(12)
        << "band_z=" << config.band_z
        << ";std_log=" << band.band_std
        << ";raw_std_log=" << band.raw_band_std
        << ";raw_calibration_var=" << band.raw_calibration_var
        << ";raw_z=" << band.raw_z
        << ";sigma_log=" << state.sigma
        << ";multiplier=" << state.detection_band_multiplier
        << ";residual_scale_ewma=" << state.residual_scale_ewma
        << ";pred_var=" << band.pred_var_component
        << ";calibrated_sigma_var=" << band.calibrated_sigma_var
        << ";extra_obs_var=" << band.extra_obs_var
        << ";maturity_var=" << band.maturity_uncertainty_var
        << ";missing_component_var=" << band.component_missing_uncertainty_var
        << ";drift_evidence=" << state.drift_evidence
        << ";level_shift_evidence=" << state.level_shift_evidence
        << ";combined_drift_evidence=" << combined_drift_evidence
        << ";level_shift_cusum_pos=" << state.level_shift_cusum_pos
        << ";level_shift_cusum_neg=" << state.level_shift_cusum_neg
        << ";cap_applied=" << (band.std_cap_applied ? 1 : 0)
        << ";std_cap=" << config.detection_band_std_cap;
    return out.str();
}

RollingBaselineResult BaseResultFromPoint(const ObservedModelPoint& point) {
    RollingBaselineResult result;
    result.status = point.status;
    result.series_key = point.series_key;
    result.bucket_id = point.bucket_id;
    result.observed = point.observed;
    result.observed_model = point.y_model;
    result.can_score = point.can_score;
    result.can_update = point.can_update;
    result.score_weight = point.score_weight;
    result.update_weight = point.update_weight;
    result.sample_count = point.sample_count;
    result.skipped_low_sample_count = point.skipped_low_sample_count;
    result.numerator = point.numerator;
    result.denominator = point.denominator;
    result.skipped_low_denominator = point.skipped_low_denominator;
    result.uncertainty_source = point.uncertainty_source;
    result.diagnostics = point.diagnostics;
    return result;
}

void FillB3StateFields(const RollingState& state, bool can_alert, RollingBaselineResult* result) {
    if (!result) return;
    result->maturity_status = RollingMaturityStatusName(state.maturity_status);
    result->score_trust_status = ScoreTrustStatusName(state.score_trust_status);
    result->calibration_status = RollingCalibrationStatusName(state.calibration_status);
    result->learning_confidence = state.learning_confidence;
    result->score_confidence = state.score_confidence;
    result->effective_confidence = state.effective_confidence;
    result->confidence = state.effective_confidence;
    result->can_alert = can_alert;
    result->enabled_components = BuildEnabledComponents(state);
    result->component_readiness = BuildComponentReadiness(state);
}

void FillResultFromDetectionBand(const BaselineTaskSpec& spec,
                                 const ObservedModelPoint& point,
                                 const DetectionBandResult& band,
                                 const RollingState& state,
                                 const BaselineRollingConfig& config,
                                 double adapt_boost,
                                 double update_weight,
                                 bool can_alert,
                                 RollingBaselineResult* result) {
    result->status = band.status;
    result->model_mu = band.detection_mu;
    result->model_lower = band.model_lower;
    result->model_upper = band.model_upper;
    result->baseline_mu = ModelToObservedMu(spec, band.detection_mu);
    result->baseline_lower = ModelToObservedLower(spec, band.model_lower);
    result->baseline_upper = ModelToObservedUpper(spec, band.model_upper);
    if (result->baseline_upper < result->baseline_lower) {
        std::swap(result->baseline_upper, result->baseline_lower);
    }
    result->band_width = result->baseline_upper - result->baseline_lower;
    result->residual = band.residual;
    result->band_std = band.band_std;
    result->z_score = band.detection_z;
    result->is_outside_band = band.is_outside_band;
    result->can_score = point.can_score;
    result->can_update = point.can_update && update_weight > 0.0;
    result->score_weight = point.score_weight;
    result->update_weight = update_weight;
    result->state_status = RollingStateStatusName(state.state_status);
    result->drift_evidence = state.drift_evidence;
    result->adapt_boost = adapt_boost;
    result->diagnostics = BuildBandDiagnostics(band, state, config);
    FillB3StateFields(state, can_alert, result);
}

void FillColdStartBand(const BaselineTaskSpec& spec,
                       const ObservedModelPoint& point,
                       const BaselineRollingConfig& config,
                       const RollingState& state,
                       RollingBaselineResult* result) {
    const double band_std = config.cold_start_band_scale * state.sigma;
    const double model_lower = point.y_model - config.band_z * band_std;
    const double model_upper = point.y_model + config.band_z * band_std;
    result->status = BaselineStatus::kOk;
    result->model_mu = point.y_model;
    result->model_lower = model_lower;
    result->model_upper = model_upper;
    result->baseline_mu = ModelToObservedMu(spec, point.y_model);
    result->baseline_lower = ModelToObservedLower(spec, model_lower);
    result->baseline_upper = ModelToObservedUpper(spec, model_upper);
    if (result->baseline_upper < result->baseline_lower) {
        std::swap(result->baseline_upper, result->baseline_lower);
    }
    result->band_width = result->baseline_upper - result->baseline_lower;
    result->band_std = band_std;
    result->residual = 0.0;
    result->z_score = 0.0;
    result->is_outside_band = false;
    result->can_score = false;
    result->can_update = true;
    result->score_weight = 0.0;
    result->update_weight = point.update_weight;
    result->confidence = state.confidence;
    result->state_status = RollingStateStatusName(state.state_status);
    FillB3StateFields(state, false, result);
    result->uncertainty_source.push_back("cold_start");
    result->uncertainty_source.push_back("first_observation");
}

BaselineStatus ResolveConfigOrStatus(const BaselineTaskSpec& spec,
                                     BaselineRollingConfig* config,
                                     std::string* diagnostics) {
    return ResolveBaselineRollingConfig(spec, config, diagnostics);
}

const BootstrapSeed* FindSeed(const BootstrapSeedStore& seeds, const std::string& series_key) {
    const auto it = seeds.find(series_key);
    return it == seeds.end() ? nullptr : &it->second;
}

template <typename Writer>
void WriteStringField(Writer* writer, const char* name, const std::string& value) {
    writer->Key(name);
    writer->String(value.c_str());
}

template <typename Writer>
void WriteTaskIdentity(Writer* writer, const BaselineTaskSpec& spec) {
    writer->Key("task_identity");
    writer->StartObject();
    WriteStringField(writer, "task_id", spec.task_id);
    WriteStringField(writer, "task_kind", spec.task_kind);
    WriteStringField(writer, "feature_id", spec.feature_id);
    WriteStringField(writer, "feature_type", spec.feature_type);
    WriteStringField(writer, "profile", spec.profile);
    writer->EndObject();
}

uint64_t EstimateStateBytes(const RollingState& state) {
    return static_cast<uint64_t>(
        sizeof(RollingState) +
        sizeof(double) * (state.theta.daily.sin_coeff.capacity() +
                          state.theta.daily.cos_coeff.capacity() +
                          state.theta.daily.sin_p.capacity() +
                          state.theta.daily.cos_p.capacity() +
                          state.theta.weekly.sin_coeff.capacity() +
                          state.theta.weekly.cos_coeff.capacity() +
                          state.theta.weekly.sin_p.capacity() +
                          state.theta.weekly.cos_p.capacity()) +
        sizeof(uint32_t) * state.monthpos_dme_count.capacity() +
        state.series_key.capacity() + state.diagnostics.capacity());
}

uint64_t EstimateStatesBytes(const RollingStateMap& states) {
    uint64_t total = 0;
    for (const auto& entry : states) total += EstimateStateBytes(entry.second);
    return total;
}

double ForecastTrendSteps(int64_t dt, const BaselineRollingConfig& config) {
    const uint64_t cap =
        config.forecast_trend_cap_buckets == 0
            ? std::max<uint64_t>(1, config.day_buckets)
            : config.forecast_trend_cap_buckets;
    return static_cast<double>(
        std::min<uint64_t>(cap, static_cast<uint64_t>(std::max<int64_t>(1, dt))));
}

double EvaluateBootstrapHarmonic(const std::vector<BootstrapHarmonicInit>& harmonic,
                                 const std::vector<double>& sin_feature,
                                 const std::vector<double>& cos_feature) {
    double value = 0.0;
    for (const BootstrapHarmonicInit& item : harmonic) {
        if (item.order <= 0) continue;
        const std::size_t index = static_cast<std::size_t>(item.order - 1);
        if (index < sin_feature.size()) value += item.sin * sin_feature[index];
        if (index < cos_feature.size()) value += item.cos * cos_feature[index];
    }
    return value;
}

bool CanUseBootstrapForecastSeed(const BootstrapSeed& seed) {
    return seed.theta_init.available &&
           seed.sigma_init.available &&
           std::isfinite(seed.sigma_init.value) &&
           seed.sigma_init.value > 0.0 &&
           seed.seed_status != BootstrapSeedStatus::kNone;
}

double ForecastCorrectionWeight(const RollingState& state) {
    double weight = state.effective_confidence;
    if (!std::isfinite(weight) || weight <= 0.0) weight = state.confidence;
    if (!std::isfinite(weight)) return 0.0;
    return std::max(0.0, std::min(1.0, weight));
}

BaselineStatus BuildFusionForecastEstimator(const RollingState& state,
                                            const BootstrapSeed& seed,
                                            int64_t bucket_id,
                                            const BaselineRollingConfig& config,
                                            RollingEstimatorResult* out) {
    if (!out || !state.has_seen_observation || !CanUseBootstrapForecastSeed(seed)) {
        return BaselineStatus::kInvalidArgument;
    }
    const int64_t dt = bucket_id - state.last_seen_bucket;
    if (dt <= 0) return BaselineStatus::kInvalidArgument;

    RollingFeatureVector feature;
    const BaselineStatus feature_status =
        BuildRollingFeatureVector(bucket_id, config, &feature);
    if (feature_status != BaselineStatus::kOk) return feature_status;

    const double bootstrap_level_at_anchor =
        seed.theta_init.level +
        seed.theta_init.trend *
            static_cast<double>(state.last_seen_bucket - seed.theta_init.reference_bucket_id);
    const double bootstrap_mu =
        seed.theta_init.level +
        seed.theta_init.trend *
            static_cast<double>(bucket_id - seed.theta_init.reference_bucket_id) +
        EvaluateBootstrapHarmonic(seed.theta_init.daily_harmonic,
                                  feature.day_sin,
                                  feature.day_cos) +
        EvaluateBootstrapHarmonic(seed.theta_init.weekly_harmonic,
                                  feature.week_sin,
                                  feature.week_cos);
    const double level_delta = state.theta.level - bootstrap_level_at_anchor;
    const double trend_delta =
        (state.theta.trend - seed.theta_init.trend) * ForecastTrendSteps(dt, config);
    const double correction_weight = ForecastCorrectionWeight(state);
    const double sigma =
        std::max(config.sigma_floor,
                 std::max(std::isfinite(state.sigma) ? state.sigma : config.sigma_floor,
                          seed.sigma_init.value));
    const double pred_var = std::max(0.0, state.p_level);
    const double obs_var = pred_var + sigma * sigma + config.sigma_floor * config.sigma_floor;
    if (!std::isfinite(obs_var) || obs_var <= 0.0) {
        return BaselineStatus::kInvalidArgument;
    }

    RollingEstimatorResult result;
    result.status = BaselineStatus::kOk;
    result.bucket_id = bucket_id;
    result.dt = dt;
    result.model_mu = bootstrap_mu + correction_weight * (level_delta + trend_delta);
    result.pred_var = pred_var;
    result.obs_var = obs_var;
    result.band_std = std::sqrt(obs_var);
    result.model_lower = result.model_mu - config.band_z * result.band_std;
    result.model_upper = result.model_mu + config.band_z * result.band_std;
    result.pred_p_level = pred_var;
    *out = result;
    return BaselineStatus::kOk;
}

void CountStatuses(const RollingStateMap& states,
                   uint64_t* cold,
                   uint64_t* warming,
                   uint64_t* ready) {
    *cold = 0;
    *warming = 0;
    *ready = 0;
    for (const auto& entry : states) {
        switch (entry.second.state_status) {
            case RollingStateStatus::kColdLearning:
                ++(*cold);
                break;
            case RollingStateStatus::kWarming:
                ++(*warming);
                break;
            case RollingStateStatus::kReadyHint:
                ++(*ready);
                break;
        }
    }
}

void CountB3Statuses(const RollingStateMap& states,
                     std::array<uint64_t, 8>* maturity,
                     std::array<uint64_t, 5>* score,
                     std::array<uint64_t, 5>* calibration) {
    maturity->fill(0);
    score->fill(0);
    calibration->fill(0);
    for (const auto& entry : states) {
        ++(*maturity)[static_cast<std::size_t>(entry.second.maturity_status)];
        ++(*score)[static_cast<std::size_t>(entry.second.score_trust_status)];
        ++(*calibration)[static_cast<std::size_t>(entry.second.calibration_status)];
    }
}

template <typename Writer>
void WriteStringArray(Writer* writer, const char* name, const std::vector<std::string>& values) {
    writer->Key(name);
    writer->StartArray();
    for (const std::string& value : values) {
        writer->String(value.c_str());
    }
    writer->EndArray();
}

template <typename Writer>
void WriteComponentReadinessObject(Writer* writer, const std::vector<std::string>& readiness) {
    writer->Key("component_readiness");
    writer->StartObject();
    for (const std::string& item : readiness) {
        const std::size_t eq = item.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::size_t colon = item.find(':', eq + 1);
        const std::string key = item.substr(0, eq);
        const std::string value =
            colon == std::string::npos ? item.substr(eq + 1) : item.substr(eq + 1, colon - eq - 1);
        WriteStringField(writer, key.c_str(), value);
    }
    writer->EndObject();
}

RollingBaselineResult SubmitObservedPoint(const BaselineTaskSpec& spec,
                                          const BootstrapSeedStore& seeds,
                                          RollingStateMap* states,
                                          const ObservedModelPoint& point,
                                          const RollingSubmitOptions& options,
                                          const BaselineRollingConfig& config) {
    RollingBaselineResult result = BaseResultFromPoint(point);
    if (!states || point.series_key.empty()) {
        result.status = BaselineStatus::kInvalidArgument;
        return result;
    }
    if (point.status != BaselineStatus::kOk) return result;

    auto state_it = states->find(point.series_key);
    if (state_it == states->end()) {
        bool initialized_from_bootstrap = false;
        if (options.allow_auto_init_from_bootstrap) {
            const BootstrapSeed* seed = FindSeed(seeds, point.series_key);
            if (seed) {
                RollingState state;
                std::string diagnostics;
                const BaselineStatus status = InitializeRollingStateFromBootstrapSeed(
                    spec, point.series_key, *seed, config, &state, &diagnostics);
                if (status != BaselineStatus::kOk) {
                    result.status = status;
                    result.diagnostics = diagnostics;
                    return result;
                }
                state_it = states->emplace(point.series_key, std::move(state)).first;
                initialized_from_bootstrap = true;
            } else if (!options.allow_auto_init_from_empty) {
                result.status = BaselineStatus::kNotTrained;
                return result;
            }
        } else if (!options.allow_auto_init_from_empty) {
            result.status = BaselineStatus::kNotTrained;
            return result;
        }

        if (!initialized_from_bootstrap) {
            if (!options.allow_auto_init_from_empty) {
                result.status = BaselineStatus::kNotTrained;
                return result;
            }
            if (!point.can_update) {
                result.status = BaselineStatus::kInsufficientData;
                return result;
            }
            RollingState state;
            BaselineStatus status = BuildEmptyRollingState(point.series_key, config, &state);
            if (status != BaselineStatus::kOk) {
                result.status = status;
                return result;
            }
            status = InitializeEmptyRollingStateFromObservation(point, config, &state);
            if (status != BaselineStatus::kOk) {
                result.status = status;
                return result;
            }
            state_it = states->emplace(point.series_key, std::move(state)).first;
            FillColdStartBand(spec, point, config, state_it->second, &result);
            return result;
        }
    }

    RollingState& state = state_it->second;
    RollingEstimatorResult estimator;
    BaselineStatus status = PredictRollingState(state, point, config, &estimator);
    if (status != BaselineStatus::kOk) {
        result.status = status;
        return result;
    }

    const bool monthpos_ready = state.monthpos_status == RollingMonthposStatus::kMonthlyReady;
    const double active_monthpos_effect =
        monthpos_ready ? EvaluateRollingMonthpos(state, point.bucket_id, config) : 0.0;
    DetectionBandResult detection_band;
    status = BuildDetectionBand(
        state, point, estimator, config, active_monthpos_effect, &detection_band);
    if (status != BaselineStatus::kOk) {
        result.status = status;
        return result;
    }

    DriftAdaptResult drift;
    status = UpdateDriftEvidence(estimator.residual,
                                 detection_band.band_std,
                                 point.can_score,
                                 config,
                                 &state,
                                 &drift);
    if (status != BaselineStatus::kOk) {
        result.status = status;
        return result;
    }

    status = UpdateDetectionCalibration(point, detection_band, config, &state);
    if (status != BaselineStatus::kOk) {
        result.status = status;
        return result;
    }

    ScoreTrustResult score_trust;
    status =
        UpdateScoreTrust(point, detection_band.detection_z, config, &state, &score_trust);
    if (status != BaselineStatus::kOk) {
        result.status = status;
        return result;
    }
    const RollingState result_state = state;

    UpdateGateResult gate = ComputeUpdateGate(estimator.z_score, drift.adapt_boost, config);
    ObservedModelPoint weighted = point;
    if (monthpos_ready) {
        weighted.y_model -= active_monthpos_effect;
    }
    weighted.update_weight *= gate.gate_update_weight;
    const double base_update_weight = weighted.can_update ? weighted.update_weight : 0.0;

    if (base_update_weight > 0.0) {
        RollingEstimatorResult update_result;
        status = UpdateRollingStateWithObservation(
            weighted, config, &state, &update_result, drift.adapt_boost);
        if (status != BaselineStatus::kOk) {
            result.status = status;
            return result;
        }
        estimator = update_result;
        ResidualScaleResult scale;
        status = UpdateResidualScale(estimator.residual, base_update_weight, config, &state, &scale);
        if (status != BaselineStatus::kOk) {
            result.status = status;
            return result;
        }

        status = UpdateMaturityEvidence(weighted, config, &state);
        if (status != BaselineStatus::kOk) {
            result.status = status;
            return result;
        }
        if (state.score_trust_status != ScoreTrustStatus::kDriftLearning) {
            status = UpdateRollingMonthpos(
                point, estimator.residual, base_update_weight, config, &state);
            if (status != BaselineStatus::kOk) {
                result.status = status;
                return result;
            }
        }
    }

    FillResultFromDetectionBand(spec,
                                point,
                                detection_band,
                                result_state,
                                config,
                                drift.adapt_boost,
                                base_update_weight,
                                score_trust.can_alert,
                                &result);
    if (gate.skip_update) {
        result.uncertainty_source.push_back("anomaly_skip_update");
    } else if (gate.downweight_update) {
        result.uncertainty_source.push_back("anomaly_downweight_update");
    }
    return result;
}

}  // namespace

RollingBaselineResult RunValueRollingSubmit(const BaselineTaskSpec& spec,
                                            const BootstrapSeedStore& seeds,
                                            RollingStateMap* states,
                                            const ValueRollingObservation& obs,
                                            const RollingSubmitOptions& options) {
    BaselineRollingConfig config;
    std::string diagnostics;
    RollingBaselineResult result;
    result.series_key = obs.series_key;
    result.bucket_id = obs.bucket_id;
    result.observed = obs.value;
    result.sample_count = obs.sample_count;
    const BaselineStatus config_status = ResolveConfigOrStatus(spec, &config, &diagnostics);
    if (config_status != BaselineStatus::kOk) {
        result.status = config_status;
        result.diagnostics = diagnostics;
        return result;
    }

    const ObservedModelPoint point = AdaptValueRollingObservation(spec, config, obs);
    return SubmitObservedPoint(spec, seeds, states, point, options, config);
}

RollingBaselineResult RunRatioRollingSubmit(const BaselineTaskSpec& spec,
                                            const BootstrapSeedStore& seeds,
                                            RollingStateMap* states,
                                            const RatioRollingObservation& obs,
                                            const RollingSubmitOptions& options) {
    BaselineRollingConfig config;
    std::string diagnostics;
    RollingBaselineResult result;
    result.series_key = obs.series_key;
    result.bucket_id = obs.bucket_id;
    result.numerator = obs.numerator;
    result.denominator = obs.denominator;
    const BaselineStatus config_status = ResolveConfigOrStatus(spec, &config, &diagnostics);
    if (config_status != BaselineStatus::kOk) {
        result.status = config_status;
        result.diagnostics = diagnostics;
        return result;
    }

    const ObservedModelPoint point = AdaptRatioRollingObservation(spec, config, obs);
    return SubmitObservedPoint(spec, seeds, states, point, options, config);
}

RollingPrediction PredictRollingForSeries(const BaselineTaskSpec& spec,
                                          const BootstrapSeedStore& seeds,
                                          const RollingStateMap& states,
                                          std::string_view series_key,
                                          int64_t bucket_id) {
    RollingPrediction result;
    const std::string key(series_key);
    if (key.empty()) {
        result.status = BaselineStatus::kInvalidArgument;
        return result;
    }
    const auto it = states.find(key);
    if (it == states.end()) {
        result.status = BaselineStatus::kNotTrained;
        return result;
    }
    if (bucket_id <= it->second.last_seen_bucket) {
        result.status = BaselineStatus::kInvalidArgument;
        return result;
    }

    BaselineRollingConfig config;
    std::string diagnostics;
    const BaselineStatus config_status = ResolveConfigOrStatus(spec, &config, &diagnostics);
    if (config_status != BaselineStatus::kOk) {
        result.status = config_status;
        return result;
    }

    RollingEstimatorResult estimator;
    BaselineStatus predict_status = BaselineStatus::kNotTrained;
    const BootstrapSeed* seed = FindSeed(seeds, key);
    if (seed && CanUseBootstrapForecastSeed(*seed)) {
        predict_status =
            BuildFusionForecastEstimator(it->second, *seed, bucket_id, config, &estimator);
    }
    if (predict_status != BaselineStatus::kOk) {
        predict_status = PredictRollingForecastState(it->second, bucket_id, config, &estimator);
    }
    if (predict_status != BaselineStatus::kOk) {
        result.status = predict_status;
        return result;
    }

    const bool monthpos_ready =
        it->second.monthpos_status == RollingMonthposStatus::kMonthlyReady;
    const double active_monthpos_effect =
        monthpos_ready ? EvaluateRollingMonthpos(it->second, bucket_id, config) : 0.0;
    const double forecast_model_mu = estimator.model_mu + active_monthpos_effect;
    const double forecast_model_lower =
        forecast_model_mu - config.forecast_band_z * estimator.band_std;
    const double forecast_model_upper =
        forecast_model_mu + config.forecast_band_z * estimator.band_std;
    result.baseline_mu = ModelToObservedMu(spec, forecast_model_mu);
    result.baseline_lower =
        ModelToObservedLower(spec, forecast_model_lower);
    result.baseline_upper =
        ModelToObservedUpper(spec, forecast_model_upper);
    result.band_z = config.forecast_band_z;
    if (result.baseline_upper < result.baseline_lower) {
        std::swap(result.baseline_upper, result.baseline_lower);
    }
    return result;
}

RollingWarmupStats WarmupRollingStatesFromBootstrapSeeds(const BaselineTaskSpec& spec,
                                                         const BootstrapSeedStore& seeds,
                                                         RollingStateMap* states) {
    RollingWarmupStats stats;
    if (!states || seeds.empty()) return stats;

    BaselineRollingConfig config;
    std::string diagnostics;
    if (ResolveBaselineRollingConfig(spec, &config, &diagnostics) != BaselineStatus::kOk) {
        stats.failure_count += static_cast<uint64_t>(seeds.size());
        return stats;
    }

    for (const auto& entry : seeds) {
        if (states->find(entry.first) != states->end()) {
            ++stats.skipped_existing_count;
            continue;
        }
        RollingState state;
        const BaselineStatus status = InitializeRollingStateFromBootstrapSeed(
            spec, entry.first, entry.second, config, &state, nullptr);
        if (status == BaselineStatus::kOk) {
            states->emplace(entry.first, std::move(state));
            ++stats.success_count;
        } else {
            ++stats.failure_count;
        }
    }
    return stats;
}

BaselineSerializationResult QueryRollingTaskSnapshot(const BaselineTaskSpec& spec,
                                                     const RollingStateMap& states,
                                                     BaselineSerializationFormat format) {
    if (format != BaselineSerializationFormat::kJson) {
        return {BaselineStatus::kUnsupportedFormat, ""};
    }

    uint64_t cold = 0;
    uint64_t warming = 0;
    uint64_t ready = 0;
    CountStatuses(states, &cold, &warming, &ready);
    std::array<uint64_t, 8> maturity_counts{};
    std::array<uint64_t, 5> score_counts{};
    std::array<uint64_t, 5> calibration_counts{};
    CountB3Statuses(states, &maturity_counts, &score_counts, &calibration_counts);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("schema_version");
    writer.Int(1);
    WriteStringField(&writer, "document_kind", "rolling_task_snapshot");
    WriteTaskIdentity(&writer, spec);
    writer.Key("rolling_series_count");
    writer.Uint64(static_cast<uint64_t>(states.size()));
    writer.Key("state_status_counts");
    writer.StartObject();
    writer.Key("cold_learning");
    writer.Uint64(cold);
    writer.Key("warming");
    writer.Uint64(warming);
    writer.Key("ready_hint");
    writer.Uint64(ready);
    writer.EndObject();
    writer.Key("maturity_status_counts");
    writer.StartObject();
    writer.Key("cold_learning");
    writer.Uint64(maturity_counts[static_cast<std::size_t>(RollingMaturityStatus::kColdLearning)]);
    writer.Key("level_ready");
    writer.Uint64(maturity_counts[static_cast<std::size_t>(RollingMaturityStatus::kLevelReady)]);
    writer.Key("daily_warming");
    writer.Uint64(maturity_counts[static_cast<std::size_t>(RollingMaturityStatus::kDailyWarming)]);
    writer.Key("daily_ready");
    writer.Uint64(maturity_counts[static_cast<std::size_t>(RollingMaturityStatus::kDailyReady)]);
    writer.Key("weekly_warming");
    writer.Uint64(maturity_counts[static_cast<std::size_t>(RollingMaturityStatus::kWeeklyWarming)]);
    writer.Key("weekly_ready");
    writer.Uint64(maturity_counts[static_cast<std::size_t>(RollingMaturityStatus::kWeeklyReady)]);
    writer.Key("monthly_warming");
    writer.Uint64(maturity_counts[static_cast<std::size_t>(RollingMaturityStatus::kMonthlyWarming)]);
    writer.Key("monthly_ready");
    writer.Uint64(maturity_counts[static_cast<std::size_t>(RollingMaturityStatus::kMonthlyReady)]);
    writer.EndObject();
    writer.Key("score_trust_status_counts");
    writer.StartObject();
    writer.Key("score_untrusted");
    writer.Uint64(score_counts[static_cast<std::size_t>(ScoreTrustStatus::kScoreUntrusted)]);
    writer.Key("score_warming");
    writer.Uint64(score_counts[static_cast<std::size_t>(ScoreTrustStatus::kScoreWarming)]);
    writer.Key("score_ready");
    writer.Uint64(score_counts[static_cast<std::size_t>(ScoreTrustStatus::kScoreReady)]);
    writer.Key("drift_learning");
    writer.Uint64(score_counts[static_cast<std::size_t>(ScoreTrustStatus::kDriftLearning)]);
    writer.Key("recalibrating");
    writer.Uint64(score_counts[static_cast<std::size_t>(ScoreTrustStatus::kRecalibrating)]);
    writer.EndObject();
    writer.Key("calibration_status_counts");
    writer.StartObject();
    writer.Key("uncalibrated");
    writer.Uint64(calibration_counts[static_cast<std::size_t>(RollingCalibrationStatus::kUncalibrated)]);
    writer.Key("warming");
    writer.Uint64(calibration_counts[static_cast<std::size_t>(RollingCalibrationStatus::kWarming)]);
    writer.Key("calibrated");
    writer.Uint64(calibration_counts[static_cast<std::size_t>(RollingCalibrationStatus::kCalibrated)]);
    writer.Key("expanding");
    writer.Uint64(calibration_counts[static_cast<std::size_t>(RollingCalibrationStatus::kExpanding)]);
    writer.Key("recalibrating");
    writer.Uint64(calibration_counts[static_cast<std::size_t>(RollingCalibrationStatus::kRecalibrating)]);
    writer.EndObject();
    writer.Key("rolling_state_created_total");
    writer.Uint64(static_cast<uint64_t>(states.size()));
    writer.Key("rolling_state_evicted_total");
    writer.Uint64(0);
    writer.Key("rolling_state_memory_estimate_bytes");
    writer.Uint64(EstimateStatesBytes(states));
    WriteStringField(&writer, "diagnostics", "");
    writer.EndObject();
    return {BaselineStatus::kOk, buf.GetString()};
}

BaselineSerializationResult QueryRollingSeriesSnapshot(const BaselineTaskSpec& spec,
                                                       const RollingStateMap& states,
                                                       std::string_view series_key,
                                                       BaselineSerializationFormat format) {
    if (format != BaselineSerializationFormat::kJson) {
        return {BaselineStatus::kUnsupportedFormat, ""};
    }
    const std::string key(series_key);
    const auto it = states.find(key);
    if (key.empty()) return {BaselineStatus::kInvalidArgument, ""};
    if (it == states.end()) return {BaselineStatus::kNotTrained, ""};
    const RollingState& state = it->second;

    const double band_std = std::max(state.sigma, 0.0);
    const double model_lower = state.theta.level - 3.0 * band_std;
    const double model_upper = state.theta.level + 3.0 * band_std;
    const double baseline_mu = ModelToObservedMu(spec, state.theta.level);
    double baseline_lower = ModelToObservedLower(spec, model_lower);
    double baseline_upper = ModelToObservedUpper(spec, model_upper);
    if (baseline_upper < baseline_lower) std::swap(baseline_upper, baseline_lower);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("schema_version");
    writer.Int(1);
    WriteStringField(&writer, "document_kind", "rolling_series_snapshot");
    WriteTaskIdentity(&writer, spec);
    writer.Key("series_identity");
    writer.StartObject();
    WriteStringField(&writer, "series_key", key);
    writer.EndObject();
    WriteStringField(&writer, "state_status", RollingStateStatusName(state.state_status));
    WriteStringField(&writer, "maturity_status", RollingMaturityStatusName(state.maturity_status));
    WriteStringField(&writer, "score_trust_status", ScoreTrustStatusName(state.score_trust_status));
    WriteStringField(&writer, "calibration_status", RollingCalibrationStatusName(state.calibration_status));
    writer.Key("has_seen_observation");
    writer.Bool(state.has_seen_observation);
    writer.Key("last_seen_bucket");
    writer.Int64(state.last_seen_bucket);
    writer.Key("accepted_update_count");
    writer.Uint64(state.accepted_update_count);
    writer.Key("confidence");
    writer.Double(state.effective_confidence);
    writer.Key("learning_confidence");
    writer.Double(state.learning_confidence);
    writer.Key("score_confidence");
    writer.Double(state.score_confidence);
    writer.Key("effective_confidence");
    writer.Double(state.effective_confidence);
    writer.Key("band");
    writer.StartObject();
    writer.Key("baseline_mu");
    writer.Double(baseline_mu);
    writer.Key("baseline_lower");
    writer.Double(baseline_lower);
    writer.Key("baseline_upper");
    writer.Double(baseline_upper);
    writer.Key("band_width");
    writer.Double(baseline_upper - baseline_lower);
    writer.EndObject();
    writer.Key("control");
    writer.StartObject();
    writer.Key("can_score");
    writer.Bool(state.has_seen_observation);
    writer.Key("can_update");
    writer.Bool(state.has_seen_observation);
    writer.Key("update_weight");
    writer.Double(0.0);
    writer.Key("drift_evidence");
    writer.Double(state.drift_evidence);
    writer.Key("level_shift_evidence");
    writer.Double(state.level_shift_evidence);
    writer.Key("combined_drift_evidence");
    writer.Double(std::fabs(state.drift_evidence) >= std::fabs(state.level_shift_evidence)
                     ? state.drift_evidence
                     : state.level_shift_evidence);
    writer.EndObject();
    writer.Key("maturity");
    writer.StartObject();
    WriteStringField(&writer, "status", RollingMaturityStatusName(state.maturity_status));
    writer.Key("learning_confidence");
    writer.Double(state.learning_confidence);
    WriteStringArray(&writer, "enabled_components", BuildEnabledComponents(state));
    WriteComponentReadinessObject(&writer, BuildComponentReadiness(state));
    writer.Key("coverage");
    writer.StartObject();
    writer.Key("daily_ratio");
    writer.Double(DailyCoverageRatio(state));
    writer.Key("weekly_ratio");
    writer.Double(WeeklyCoverageRatio(state));
    writer.Key("monthpos_ratio");
    writer.Double(MonthposCoverageRatio(state));
    writer.EndObject();
    writer.EndObject();
    writer.Key("score_trust");
    writer.StartObject();
    WriteStringField(&writer, "status", ScoreTrustStatusName(state.score_trust_status));
    writer.Key("score_confidence");
    writer.Double(state.score_confidence);
    writer.Key("can_alert");
    writer.Bool(state.score_trust_status == ScoreTrustStatus::kScoreReady &&
                MaturityAtLeast(state.maturity_status, RollingMaturityStatus::kDailyReady));
    WriteStringField(&writer, "reason", state.degradation_reason);
    writer.EndObject();
    writer.Key("calibration");
    writer.StartObject();
    WriteStringField(&writer, "status", RollingCalibrationStatusName(state.calibration_status));
    writer.Key("band_multiplier");
    writer.Double(state.detection_band_multiplier);
    writer.Key("coverage_ewma");
    writer.Double(state.coverage_ewma);
    writer.Key("tail3_ewma");
    writer.Double(state.tail3_ewma);
    writer.Key("tail5_ewma");
    writer.Double(state.tail5_ewma);
    writer.Key("abs_z_ewma");
    writer.Double(state.abs_z_ewma);
    writer.Key("calibration_update_count");
    writer.Uint64(state.calibration_update_count);
    writer.EndObject();
    writer.Key("monthpos");
    writer.StartObject();
    WriteStringField(&writer, "status", RollingMonthposStatusName(state.monthpos_status));
    writer.Key("month_transition_count");
    writer.Uint64(state.month_transition_count);
    writer.Key("ready_count");
    writer.Uint64(state.monthpos_ready_count);
    writer.EndObject();
    writer.Key("state_size_bytes");
    writer.Uint64(EstimateStateBytes(state));
    WriteStringField(&writer, "diagnostics", state.diagnostics);
    writer.EndObject();
    return {BaselineStatus::kOk, buf.GetString()};
}

}  // namespace baseline
}  // namespace flowsql
