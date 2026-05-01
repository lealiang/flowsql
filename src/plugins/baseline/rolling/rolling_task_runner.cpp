/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/rolling/rolling_task_runner.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "plugins/baseline/rolling/drift_adapt.h"
#include "plugins/baseline/rolling/observation_adapter.h"
#include "plugins/baseline/rolling/residual_scale.h"
#include "plugins/baseline/rolling/rolling_config.h"
#include "plugins/baseline/rolling/rolling_estimator.h"
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

void FillResultFromEstimator(const BaselineTaskSpec& spec,
                             const ObservedModelPoint& point,
                             const RollingEstimatorResult& estimator,
                             const RollingState& state,
                             double adapt_boost,
                             double update_weight,
                             RollingBaselineResult* result) {
    result->status = estimator.status;
    result->model_mu = estimator.model_mu;
    result->model_lower = estimator.model_lower;
    result->model_upper = estimator.model_upper;
    result->baseline_mu = ModelToObservedMu(spec, estimator.model_mu);
    result->baseline_lower = ModelToObservedLower(spec, estimator.model_lower);
    result->baseline_upper = ModelToObservedUpper(spec, estimator.model_upper);
    if (result->baseline_upper < result->baseline_lower) {
        std::swap(result->baseline_upper, result->baseline_lower);
    }
    result->band_width = result->baseline_upper - result->baseline_lower;
    result->residual = estimator.residual;
    result->band_std = estimator.band_std;
    result->z_score = estimator.z_score;
    result->is_outside_band = point.can_score &&
        (point.y_model < estimator.model_lower || point.y_model > estimator.model_upper);
    result->can_score = point.can_score;
    result->can_update = point.can_update && update_weight > 0.0;
    result->score_weight = point.score_weight;
    result->update_weight = update_weight;
    result->confidence = state.confidence;
    result->state_status = RollingStateStatusName(state.state_status);
    result->drift_evidence = state.drift_evidence;
    result->adapt_boost = adapt_boost;
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
        state.series_key.capacity() + state.diagnostics.capacity());
}

uint64_t EstimateStatesBytes(const RollingStateMap& states) {
    uint64_t total = 0;
    for (const auto& entry : states) total += EstimateStateBytes(entry.second);
    return total;
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

    DriftAdaptResult drift;
    status = UpdateDriftEvidence(estimator.residual,
                                 estimator.band_std,
                                 point.can_score,
                                 config,
                                 &state,
                                 &drift);
    if (status != BaselineStatus::kOk) {
        result.status = status;
        return result;
    }

    UpdateGateResult gate = ComputeUpdateGate(estimator.z_score, drift.adapt_boost, config);
    ObservedModelPoint weighted = point;
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
    }

    FillResultFromEstimator(
        spec, point, estimator, state, drift.adapt_boost, base_update_weight, &result);
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

    ObservedModelPoint point;
    point.status = BaselineStatus::kOk;
    point.series_key = key;
    point.bucket_id = bucket_id;
    point.y_model = 0.0;
    point.can_score = false;
    point.can_update = false;

    RollingEstimatorResult estimator;
    const BaselineStatus predict_status =
        PredictRollingState(it->second, point, config, &estimator);
    if (predict_status != BaselineStatus::kOk) {
        result.status = predict_status;
        return result;
    }

    result.baseline_mu = ModelToObservedMu(spec, estimator.model_mu);
    result.baseline_lower = ModelToObservedLower(spec, estimator.model_lower);
    result.baseline_upper = ModelToObservedUpper(spec, estimator.model_upper);
    result.band_z = config.band_z;
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
    writer.Key("has_seen_observation");
    writer.Bool(state.has_seen_observation);
    writer.Key("last_seen_bucket");
    writer.Int64(state.last_seen_bucket);
    writer.Key("accepted_update_count");
    writer.Uint64(state.accepted_update_count);
    writer.Key("confidence");
    writer.Double(state.confidence);
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
    writer.EndObject();
    writer.Key("state_size_bytes");
    writer.Uint64(EstimateStateBytes(state));
    WriteStringField(&writer, "diagnostics", state.diagnostics);
    writer.EndObject();
    return {BaselineStatus::kOk, buf.GetString()};
}

}  // namespace baseline
}  // namespace flowsql
