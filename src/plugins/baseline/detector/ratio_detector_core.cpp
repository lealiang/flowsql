/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/detector/ratio_detector_core.h"

#include <common/error_code.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "plugins/baseline/common/result_builder.h"

namespace flowsql {
namespace baseline {

namespace {

constexpr uint32_t kT2TrainMinRateCore = 50;
constexpr uint32_t kT2TrainMinRatioBursty = 100;
constexpr double kT2PriorRateCore = 2.0;
constexpr double kT2PriorRatioBursty = 4.0;
constexpr double kT2PhiOverRateCore = 1.5;
constexpr double kT2PhiOverRatioBursty = 2.0;
constexpr const char* kCandidateStateTrained = "trained";
constexpr double kRatioVarianceFloor = 0.25;
constexpr double kRatioShiftWeight = 0.8;
constexpr double kPointWarn = 3.0;
constexpr double kPointCrit = 5.0;
constexpr double kShadowConfidenceCap = 0.8;
constexpr double kShadowScoreScale = 1.5;
constexpr uint32_t kMinShiftConfirmForRebuild = 3;
constexpr DriftConfig kDriftConfig{};

std::string CopyKey(const BaselineStringRef& key) {
    if (!key.data || key.size == 0) return "";
    return std::string(key.data, key.size);
}

void FillBadRequestResult(DetectorResult* out) {
    if (!out) return;
    *out = DetectorResult{};
    out->status = error::BAD_REQUEST;
}

struct SelectedRatioBaseline {
    bool ready = false;
    BaselineProvider provider = BaselineProvider::kFormal;
    BaselineSourceDecision decision;
    FormalPrediction prediction;
    ShadowRefKind shadow_ref_kind = ShadowRefKind::kNone;
    std::shared_ptr<RatioFormalModel> model;
};

const EventCalendarSpec* TaskEventCalendar(const RatioDetectorCoreSpec& spec) {
    return spec.event_calendar_spec ? &(*spec.event_calendar_spec) : nullptr;
}

bool BuildProfile(const RatioDetectorCoreSpec& spec,
                  RatioFeatureProfile* out,
                  std::string* err) {
    if (!out) {
        if (err) *err = "profile output must not be null";
        return false;
    }

    RatioFeatureProfile profile;
    profile.feature_type = spec.feature_type;
    profile.feature_profile = spec.feature_profile;

    if (spec.feature_profile == "rate_core") {
        profile.d_min_train = kT2TrainMinRateCore;
        profile.s_prior = kT2PriorRateCore;
        profile.phi_over = kT2PhiOverRateCore;
    } else if (spec.feature_profile == "ratio_bursty") {
        profile.d_min_train = kT2TrainMinRatioBursty;
        profile.s_prior = kT2PriorRatioBursty;
        profile.phi_over = kT2PhiOverRatioBursty;
    } else {
        if (err) *err = "unsupported t2 feature_profile";
        return false;
    }

    profile.d_score_min = (profile.d_min_train + 1) / 2;
    profile.d_shift_min = profile.d_min_train * 2;
    profile.kappa_den = static_cast<double>(profile.d_min_train);

    *out = std::move(profile);
    return true;
}

int ValidateObservation(const RatioFeatureProfile&,
                        const RatioObservation& obs,
                        std::string* err) {
    if (!obs.key.data || obs.key.size == 0) {
        if (err) *err = "key must not be empty";
        return error::BAD_REQUEST;
    }
    if (obs.bucket_id < 0) {
        if (err) *err = "bucket_id must be >= 0";
        return error::BAD_REQUEST;
    }
    if (obs.numerator < 0) {
        if (err) *err = "numerator must be >= 0";
        return error::BAD_REQUEST;
    }
    if (obs.denominator <= 0) {
        if (err) *err = "denominator must be > 0";
        return error::BAD_REQUEST;
    }
    return error::OK;
}

double ComputeObservedRatio(const RatioObservation& obs) {
    return obs.numerator / obs.denominator;
}

double ComputeRho(const RatioFeatureProfile& profile, double denominator) {
    return std::sqrt(1.0 + profile.kappa_den / denominator);
}

bool PredictServiceableModel(const RatioSeriesRuntimeState& state,
                             const EventCalendarSpec* task_calendar,
                             int64_t bucket_id,
                             FormalPrediction* out_prediction,
                             std::shared_ptr<RatioFormalModel>* out_model,
                             ShadowRefKind* out_ref_kind,
                             bool source_kind) {
    FormalPrediction prediction;
    if (state.formal_state.formal_ready &&
        PredictFormalModel(state.formal_model.get(), task_calendar, bucket_id, &prediction) ==
            error::OK &&
        prediction.ready) {
        if (out_prediction) *out_prediction = prediction;
        if (out_model) *out_model = state.formal_model;
        if (out_ref_kind) {
            *out_ref_kind = source_kind ? ShadowRefKind::kSourceFormal
                                        : ShadowRefKind::kSelfFormal;
        }
        return true;
    }

    if (state.formal_state.candidate_state == kCandidateStateTrained &&
        PredictFormalModel(state.candidate_model.get(), task_calendar, bucket_id, &prediction) ==
            error::OK &&
        prediction.ready) {
        if (out_prediction) *out_prediction = prediction;
        if (out_model) *out_model = state.candidate_model;
        if (out_ref_kind) {
            *out_ref_kind = source_kind ? ShadowRefKind::kSourceCandidate
                                        : ShadowRefKind::kSelfCandidate;
        }
        return true;
    }

    return false;
}

SelectedRatioBaseline ResolveServiceableBaseline(
    const std::string& key,
    int64_t bucket_id,
    const EventCalendarSpec* task_calendar,
    const std::unordered_map<std::string, RatioSeriesRuntimeState>& runtime_by_key,
    const std::unordered_map<std::string, BaselineSourceConfig>& series_override_map) {
    SelectedRatioBaseline selected;

    // T2 与 T1 共用相同的基线来源优先级：先看 self，再按静态配置尝试来源 key。
    // 每一级都优先 formal，只有 formal 不可服务时才允许使用已训练 candidate。
    auto self_it = runtime_by_key.find(key);
    if (self_it != runtime_by_key.end() &&
        PredictServiceableModel(self_it->second,
                                task_calendar,
                                bucket_id,
                                &selected.prediction,
                                &selected.model,
                                &selected.shadow_ref_kind,
                                false)) {
        selected.ready = true;
        selected.provider = BaselineProvider::kFormal;
        selected.decision.kind = BaselineSourceDecisionKind::kSelf;
        return selected;
    }

    auto override_it = series_override_map.find(key);
    if (override_it == series_override_map.end()) return selected;

    for (const auto& source_ref : override_it->second) {
        auto source_it = runtime_by_key.find(source_ref.source_key);
        if (source_it == runtime_by_key.end()) continue;
        if (!PredictServiceableModel(source_it->second,
                                     task_calendar,
                                     bucket_id,
                                     &selected.prediction,
                                     &selected.model,
                                     &selected.shadow_ref_kind,
                                     true)) {
            continue;
        }

        selected.ready = true;
        selected.provider = BaselineProvider::kSource;
        selected.decision.kind = BaselineSourceDecisionKind::kConfiguredSource;
        selected.decision.source_key = source_ref.source_key;
        return selected;
    }

    return selected;
}

double ComputePointScoreFromAbsResidual(double abs_residual) {
    if (abs_residual <= kPointWarn) return 0.0;
    return ClipUnit((abs_residual - kPointWarn) / (kPointCrit - kPointWarn));
}

double ComputeNormalizedScore(double score_point, double score_shift) {
    return 1.0 - (1.0 - score_point) * (1.0 - kRatioShiftWeight * score_shift);
}

BaselineReasonCode DriftReasonCode(DriftDirection direction) {
    switch (direction) {
        case DriftDirection::kUp:
            return BaselineReasonCode::kBaselineShiftUp;
        case DriftDirection::kDown:
            return BaselineReasonCode::kBaselineShiftDown;
        case DriftDirection::kNone:
            break;
    }
    return BaselineReasonCode::kUnknown;
}

}  // namespace

RatioDetectorCore::RatioDetectorCore(const RatioDetectorCoreSpec& spec)
    : spec_(spec) {
    std::string err;
    if (!BuildProfile(spec_, &profile_, &err)) {
        profile_.feature_type = spec_.feature_type;
        profile_.feature_profile = spec_.feature_profile;
    }

    for (const auto& series_override : spec_.series_overrides) {
        series_override_map_.emplace(series_override.key, series_override.baseline_sources);
    }
}

int RatioDetectorCore::Submit(const RatioObservation& obs,
                              DetectorSubmitOutput* out_submit) {
    if (!out_submit) return error::BAD_REQUEST;
    *out_submit = DetectorSubmitOutput{};
    out_submit->rebuild_intent.routed_feature_id = spec_.routed_feature_id;

    std::string err;
    int rc = ValidateObservation(profile_, obs, &err);
    if (rc != error::OK) {
        FillBadRequestResult(&out_submit->detector_result);
        return rc;
    }

    const double observed_ratio = ComputeObservedRatio(obs);
    const double rho_t = ComputeRho(profile_, obs.denominator);
    const bool gate_score = obs.denominator >= static_cast<double>(profile_.d_score_min);
    const bool gate_shift = obs.denominator >= static_cast<double>(profile_.d_shift_min);
    const std::string key = CopyKey(obs.key);

    SeriesUpdateResult update;
    rc = series_store_.ApplyObservation(
        obs.key,
        obs.bucket_id,
        SeriesPersistenceMode::kFreeze,
        false,
        &update);
    if (rc != error::OK) {
        FillBaseResult(update, &out_submit->detector_result);
        out_submit->detector_result.status = rc;
        return rc;
    }

    FillBaseResult(update, &out_submit->detector_result);
    out_submit->detector_result.status = error::OK;
    out_submit->detector_result.raw_score = 0.0;
    out_submit->detector_result.normalized_score = 0.0;
    out_submit->detector_result.confidence = 0.0;
    out_submit->detector_result.direction = BaselineDirection::kUnknown;
    out_submit->detector_result.severity = BaselineSeverity::kInfo;
    out_submit->detector_result.provider = BaselineProvider::kFormal;
    out_submit->detector_result.reason = BaselineReasonCode::kUnknown;

    bool enqueue_rebuild = false;
    int64_t rebuild_start_hint = 0;
    {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        auto& runtime_state = runtime_by_key_[key];
        double residual = 0.0;
        double score_point = 0.0;
        double score_shift = 0.0;
        DriftDirection drift_direction = runtime_state.drift_state.direction;
        bool serviceable = false;
        bool shadow_active = false;
        BaselineProvider confidence_provider = BaselineProvider::kFormal;

        if (runtime_state.shadow_state.active && update.gap > kDriftConfig.g_reset) {
            runtime_state.shadow_state.Reset();
        }

        // shadow 分支直接沿用冻结参考模型的概率预测，再叠加在线 delta。
        // 对 T2 来说，delta 作用在概率空间，最终仍然按低分母放大的方差层做标准化。
        if (runtime_state.shadow_state.active &&
            runtime_state.shadow_state.frozen_ref_model) {
            FormalPrediction shadow_prediction;
            if (PredictFormalModel(runtime_state.shadow_state.frozen_ref_model.get(),
                                   TaskEventCalendar(spec_),
                                   obs.bucket_id,
                                   &shadow_prediction) == error::OK &&
                shadow_prediction.ready) {
                serviceable = true;
                shadow_active = true;
                const double p_shadow =
                    std::min(1.0 - 1e-6,
                             std::max(1e-6,
                                      shadow_prediction.value + runtime_state.shadow_state.delta));
                const double shadow_var = std::max(
                    kRatioVarianceFloor,
                    profile_.phi_over * obs.denominator * p_shadow * (1.0 - p_shadow));
                residual =
                    (obs.numerator - obs.denominator * p_shadow) / std::sqrt(shadow_var);

                runtime_state.model_state =
                    ShadowRefUsesSource(runtime_state.shadow_state.ref_kind)
                        ? "shadow_source"
                        : "shadow_self";
                runtime_state.baseline_source.kind =
                    ShadowRefUsesSource(runtime_state.shadow_state.ref_kind)
                        ? BaselineSourceDecisionKind::kConfiguredSource
                        : BaselineSourceDecisionKind::kSelf;
                runtime_state.baseline_source.source_key =
                    runtime_state.shadow_state.ref_source_key;
                out_submit->detector_result.provider = BaselineProvider::kShadow;
                out_submit->detector_result.flags |= kBaselineFlagShadowActive;
                confidence_provider =
                    ShadowRefUsesSource(runtime_state.shadow_state.ref_kind)
                        ? BaselineProvider::kSource
                        : BaselineProvider::kFormal;

                if (gate_score) {
                    out_submit->detector_result.raw_score =
                        std::fabs(residual) / kShadowScoreScale;
                    score_point = ComputePointScoreFromAbsResidual(
                        out_submit->detector_result.raw_score);

                    runtime_state.shadow_state.delta =
                        (1.0 - kDriftConfig.alpha) * runtime_state.shadow_state.delta +
                        kDriftConfig.alpha * (observed_ratio - shadow_prediction.value);
                }
                runtime_state.shadow_state.last_bucket_id = obs.bucket_id;
            } else {
                runtime_state.shadow_state.Reset();
            }
        }

        if (!serviceable) {
            // 常态路径先决出“当前 bucket 用谁来解释”，然后才进入残差 / 漂移评分。
            // 如果 self 与 source 都不可服务，热路径只推进公共时序状态，不制造伪异常。
            const SelectedRatioBaseline selected = ResolveServiceableBaseline(
                key,
                obs.bucket_id,
                TaskEventCalendar(spec_),
                runtime_by_key_,
                series_override_map_);
            runtime_state.baseline_source = selected.decision;
            if (selected.ready) {
                serviceable = true;
                const double p_hat =
                    std::min(1.0 - 1e-6, std::max(1e-6, selected.prediction.value));
                const double var_eff = std::max(
                    kRatioVarianceFloor,
                    profile_.phi_over * obs.denominator * p_hat * (1.0 - p_hat));
                residual =
                    (obs.numerator - obs.denominator * p_hat) / std::sqrt(var_eff);
                runtime_state.model_state =
                    selected.decision.kind == BaselineSourceDecisionKind::kSelf
                        ? "serviceable_self"
                        : "serviceable_source";
                out_submit->detector_result.provider = selected.provider;
                confidence_provider = selected.provider;

                if (gate_score) {
                    out_submit->detector_result.raw_score = std::fabs(residual);
                    score_point = ComputePointScoreFromAbsResidual(
                        out_submit->detector_result.raw_score);
                }

                const DriftUpdateResult drift_result = UpdateDriftState(
                    &runtime_state.drift_state,
                    kDriftConfig,
                    obs.bucket_id,
                    residual,
                    gate_shift);
                runtime_state.last_p_shift = drift_result.p_shift;
                runtime_state.last_shift_confirmed = drift_result.shift_confirmed;
                drift_direction = drift_result.direction;
                score_shift =
                    ClipUnit((drift_result.p_shift - kDriftConfig.p_shift_low) /
                             (kDriftConfig.p_shift_high - kDriftConfig.p_shift_low));

                // 漂移确认时先启用 shadow 桥接，并把重建起点近似回退到连续确认段开头。
                // 这样慢路径可以尽量多看到“新阶段”样本，而不是继续被旧阶段稀释。
                if (drift_result.shift_confirmed &&
                    runtime_state.drift_state.confirm_count >= kMinShiftConfirmForRebuild &&
                    !runtime_state.shadow_state.active &&
                    !runtime_state.shift_rebuild_pending &&
                    selected.model) {
                    runtime_state.shadow_state.active = true;
                    runtime_state.shadow_state.ref_kind = selected.shadow_ref_kind;
                    runtime_state.shadow_state.ref_source_key =
                        selected.decision.kind == BaselineSourceDecisionKind::kConfiguredSource
                            ? selected.decision.source_key
                            : "";
                    runtime_state.shadow_state.ref_model_version =
                        selected.prediction.model_version;
                    runtime_state.shadow_state.frozen_ref_model = selected.model;
                    runtime_state.shadow_state.delta =
                        observed_ratio - selected.prediction.value;
                    runtime_state.shadow_state.last_bucket_id = obs.bucket_id;
                    runtime_state.model_state =
                        ShadowRefUsesSource(selected.shadow_ref_kind)
                            ? "shadow_source"
                            : "shadow_self";
                    runtime_state.shift_rebuild_pending = true;
                    enqueue_rebuild = true;
                    rebuild_start_hint = std::max<int64_t>(
                        0,
                        obs.bucket_id -
                            static_cast<int64_t>(runtime_state.drift_state.confirm_count) +
                            1);

                    out_submit->detector_result.provider = BaselineProvider::kShadow;
                    out_submit->detector_result.flags |= kBaselineFlagShadowActive;
                    out_submit->detector_result.raw_score = 0.0;
                    score_point = 0.0;
                    shadow_active = true;
                    confidence_provider =
                        ShadowRefUsesSource(selected.shadow_ref_kind)
                            ? BaselineProvider::kSource
                            : BaselineProvider::kFormal;
                }
            } else {
                runtime_state.model_state = "cold_start";
                runtime_state.last_p_shift = 0.0;
                runtime_state.last_shift_confirmed = false;
            }
        }

        if (serviceable) {
            out_submit->detector_result.normalized_score =
                ComputeNormalizedScore(score_point, score_shift);
            out_submit->detector_result.direction = DirectionFromResidual(residual);
            if (score_shift > 0.0 && drift_direction != DriftDirection::kNone) {
                out_submit->detector_result.direction =
                    drift_direction == DriftDirection::kUp ? BaselineDirection::kUp
                                                           : BaselineDirection::kDown;
                out_submit->detector_result.reason = DriftReasonCode(drift_direction);
            } else {
                out_submit->detector_result.reason = ReasonFromResidual(
                    residual,
                    out_submit->detector_result.normalized_score);
            }
            out_submit->detector_result.severity = SeverityFromNormalizedScore(
                out_submit->detector_result.normalized_score);

            const double confidence_base =
                shadow_active
                    ? std::min(ConfidenceBaseForProvider(confidence_provider),
                               kShadowConfidenceCap)
                    : ConfidenceBaseForProvider(confidence_provider);
            out_submit->detector_result.confidence =
                confidence_base / std::max(rho_t, 1.0);
        }

        runtime_state.last_numerator = obs.numerator;
        runtime_state.last_denominator = obs.denominator;
        runtime_state.last_observed_ratio = observed_ratio;
        runtime_state.last_rho = rho_t;
        runtime_state.last_gate_score = gate_score;
        runtime_state.last_gate_shift = gate_shift;
    }

    if (enqueue_rebuild) {
        out_submit->rebuild_intent.required = true;
        out_submit->rebuild_intent.reason = BaselineRebuildReason::kShiftConfirmed;
        out_submit->rebuild_intent.rebuild_start_hint = rebuild_start_hint;
        out_submit->rebuild_intent.bucket_end = obs.bucket_id;
    }

    return error::OK;
}

int RatioDetectorCore::BuildSeriesSnapshot(const BaselineStringRef& key,
                                           RatioSeriesSnapshot* out_snapshot) const {
    if (!out_snapshot) return error::BAD_REQUEST;

    SeriesState series_state;
    int rc = series_store_.GetState(key, &series_state);
    if (rc != error::OK) return rc;

    RatioSeriesRuntimeState runtime_state;
    {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        auto it = runtime_by_key_.find(CopyKey(key));
        if (it != runtime_by_key_.end()) runtime_state = it->second;
    }

    RatioSeriesSnapshot snapshot;
    snapshot.series_state = series_state;
    snapshot.runtime_state = runtime_state;
    snapshot.formal_predict_status = PredictFormalModel(
        runtime_state.formal_model.get(),
        TaskEventCalendar(spec_),
        series_state.last_bucket_id,
        &snapshot.formal_prediction);
    snapshot.candidate_predict_status = PredictFormalModel(
        runtime_state.candidate_model.get(),
        TaskEventCalendar(spec_),
        series_state.last_bucket_id,
        &snapshot.candidate_prediction);
    snapshot.formal_calendar_present =
        runtime_state.formal_model &&
        !runtime_state.formal_model->metadata.calendar_id.empty() &&
        !runtime_state.formal_model->metadata.calendar_version.empty();
    snapshot.candidate_calendar_present =
        runtime_state.candidate_model &&
        !runtime_state.candidate_model->metadata.calendar_id.empty() &&
        !runtime_state.candidate_model->metadata.calendar_version.empty();

    *out_snapshot = std::move(snapshot);
    return error::OK;
}

int RatioDetectorCore::GetSeriesState(const BaselineStringRef& key,
                                      SeriesState* out_state) const {
    return series_store_.GetState(key, out_state);
}

int RatioDetectorCore::BuildRebuildContext(const std::string& key,
                                           RatioRebuildContext* out_context) const {
    if (!out_context) return error::BAD_REQUEST;

    RatioRebuildContext context;
    {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        auto it = runtime_by_key_.find(key);
        if (it != runtime_by_key_.end()) {
            context.next_model_version = it->second.formal_state.candidate_generation + 1;
            context.incumbent_formal_model = it->second.formal_model;
            context.incumbent_shadow_state = it->second.shadow_state;
        }
    }

    *out_context = std::move(context);
    return error::OK;
}

void RatioDetectorCore::ApplyFormalModel(
    const std::string& key,
    const RatioApplyFormalModelResult& apply_result) {
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    auto& runtime_state = runtime_by_key_[key];
    runtime_state.formal_state.last_replay_window = apply_result.replay_window;
    runtime_state.formal_state.last_train_window = apply_result.train_window;
    runtime_state.formal_state.last_holdout_window = apply_result.holdout_window;
    runtime_state.formal_state.last_candidate_loss = apply_result.candidate_loss;
    runtime_state.formal_state.last_incumbent_loss = apply_result.incumbent_loss;
    runtime_state.formal_state.last_validation_count = apply_result.validation_count;
    runtime_state.shift_rebuild_pending = false;

    if (apply_result.candidate_trained) {
        runtime_state.formal_state.candidate_generation =
            apply_result.candidate_generation;
        // `replace_formal_model` 的语义与 T1 相同：新 formal 一旦切换成功，
        // 就把旧 shadow / drift / candidate 状态整体丢弃，避免旧概率语义继续生效。
        if (apply_result.replace_formal_model) {
            runtime_state.formal_model = apply_result.full_model;
            runtime_state.formal_state.formal_ready = (apply_result.full_model != nullptr);
            runtime_state.formal_state.formal_model_version =
                apply_result.full_model ? apply_result.full_model->metadata.model_version : 0;
            runtime_state.formal_state.formal_model_kind =
                apply_result.full_model
                    ? FormalModelKindName(apply_result.full_model->metadata.kind)
                    : "none";
            runtime_state.formal_state.candidate_state = "none";
            runtime_state.formal_state.candidate_model_version = 0;
            runtime_state.formal_state.candidate_model_kind = "none";
            runtime_state.formal_state.switch_state = apply_result.switch_state;
            runtime_state.candidate_replay.reset();
            runtime_state.candidate_model.reset();
            runtime_state.shadow_state.Reset();
            runtime_state.drift_state.Reset();
            runtime_state.last_p_shift = 0.0;
            runtime_state.last_shift_confirmed = false;
        } else if (apply_result.full_model) {
            runtime_state.formal_model = apply_result.full_model;
            runtime_state.formal_state.formal_ready = true;
            runtime_state.formal_state.formal_model_version =
                apply_result.full_model->metadata.model_version;
            runtime_state.formal_state.formal_model_kind =
                FormalModelKindName(apply_result.full_model->metadata.kind);
            runtime_state.formal_state.candidate_state = "none";
            runtime_state.formal_state.candidate_model_version = 0;
            runtime_state.formal_state.candidate_model_kind = "none";
            runtime_state.formal_state.switch_state = apply_result.switch_state;
            runtime_state.candidate_replay.reset();
            runtime_state.candidate_model.reset();
            runtime_state.shadow_state.Reset();
            runtime_state.drift_state.Reset();
            runtime_state.last_p_shift = 0.0;
            runtime_state.last_shift_confirmed = false;
        } else {
            runtime_state.formal_state.candidate_state = "none";
            runtime_state.formal_state.candidate_model_version = 0;
            runtime_state.formal_state.candidate_model_kind = "none";
            runtime_state.formal_state.switch_state = apply_result.switch_state;
            runtime_state.candidate_replay.reset();
            runtime_state.candidate_model.reset();
        }
    } else {
        runtime_state.formal_state.candidate_state = apply_result.candidate_state;
        runtime_state.formal_state.candidate_model_version = 0;
        runtime_state.formal_state.candidate_model_kind = "none";
        runtime_state.formal_state.switch_state = "none";
        runtime_state.candidate_replay.reset();
        runtime_state.candidate_model.reset();
    }
}

void RatioDetectorCore::MarkRebuildFailure(const DetectorRebuildFailure& failure) {
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    auto& runtime_state = runtime_by_key_[failure.key];
    runtime_state.formal_state.candidate_state =
        failure.candidate_state.empty() ? "fetch_failed" : failure.candidate_state;
    runtime_state.formal_state.candidate_model_version = 0;
    runtime_state.formal_state.candidate_model_kind = "none";
    runtime_state.formal_state.switch_state = "none";
    runtime_state.formal_state.last_candidate_loss = 0.0;
    runtime_state.formal_state.last_incumbent_loss = 0.0;
    runtime_state.formal_state.last_validation_count = 0;
    runtime_state.formal_state.last_replay_window = ReplayWindowSummary{};
    runtime_state.formal_state.last_train_window = ReplayWindowSummary{};
    runtime_state.formal_state.last_holdout_window = ReplayWindowSummary{};
    runtime_state.formal_state.last_replay_window.request_bucket_start =
        failure.request_bucket_start;
    runtime_state.formal_state.last_replay_window.request_bucket_end =
        failure.request_bucket_end;
    runtime_state.shift_rebuild_pending = false;
    runtime_state.candidate_replay.reset();
    runtime_state.candidate_model.reset();
}

void RatioDetectorCore::ClearPendingRebuild(const std::string& key) {
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    auto it = runtime_by_key_.find(key);
    if (it == runtime_by_key_.end()) return;
    it->second.shift_rebuild_pending = false;
}

void RatioDetectorCore::Clear() {
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    runtime_by_key_.clear();
}

size_t RatioDetectorCore::Size() const {
    return series_store_.Size();
}

}  // namespace baseline
}  // namespace flowsql
