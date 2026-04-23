/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/detector/value_detector_core.h"

#include <common/error_code.h>

#include <cmath>
#include <limits>
#include <utility>

#include "plugins/baseline/common/result_builder.h"
namespace flowsql {
namespace baseline {

namespace {

constexpr uint32_t kT1bTrainMinContCore = 50;
constexpr uint32_t kT1bTrainMinContTail = 100;
constexpr const char* kCandidateStateTrained = "trained";
constexpr double kValueShiftWeight = 0.8;
constexpr double kPointWarn = 3.0;
constexpr double kPointCrit = 5.0;
constexpr double kShadowConfidenceCap = 0.8;
constexpr double kT1SigmaRefFloor = 1e-3;
constexpr double kShadowSigmaScale = 1.5;
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

struct SelectedValueBaseline {
    bool ready = false;
    BaselineProvider provider = BaselineProvider::kFormal;
    BaselineSourceDecision decision;
    FormalPrediction prediction;
    ShadowRefKind shadow_ref_kind = ShadowRefKind::kNone;
    std::shared_ptr<ValueFormalModel> model;
};

const EventCalendarSpec* TaskEventCalendar(const ValueDetectorCoreSpec& spec) {
    return spec.event_calendar_spec ? &(*spec.event_calendar_spec) : nullptr;
}

bool BuildProfile(const ValueDetectorCoreSpec& spec,
                  ValueFeatureProfile* out,
                  std::string* err) {
    if (!out) {
        if (err) *err = "profile output must not be null";
        return false;
    }

    ValueFeatureProfile profile;
    profile.feature_type = spec.feature_type;
    profile.feature_profile = spec.feature_profile;

    if (spec.feature_type == "t1a") {
        profile.is_t1b = false;
    } else if (spec.feature_type == "t1b") {
        profile.is_t1b = true;
        if (spec.feature_profile == "cont_core") {
            profile.n_train_min = kT1bTrainMinContCore;
        } else if (spec.feature_profile == "cont_tail") {
            profile.n_train_min = kT1bTrainMinContTail;
        } else {
            if (err) *err = "unsupported t1b feature_profile";
            return false;
        }
        profile.n_score_min = (profile.n_train_min + 1) / 2;
        profile.n_shift_min = profile.n_train_min * 2;
        profile.kappa_sample = static_cast<double>(profile.n_train_min);
    } else {
        if (err) *err = "unsupported value feature_type";
        return false;
    }

    *out = std::move(profile);
    return true;
}

int ValidateObservation(const ValueFeatureProfile& profile,
                        const ValueObservation& obs,
                        std::string* err) {
    if (!obs.key.data || obs.key.size == 0) {
        if (err) *err = "key must not be empty";
        return error::BAD_REQUEST;
    }
    if (obs.bucket_id < 0) {
        if (err) *err = "bucket_id must be >= 0";
        return error::BAD_REQUEST;
    }
    if (obs.value < 0) {
        if (err) *err = "value must be >= 0";
        return error::BAD_REQUEST;
    }
    if (profile.is_t1b && obs.sample_count == 0) {
        if (err) *err = "sample_count must be >= 1 for t1b";
        return error::BAD_REQUEST;
    }
    return error::OK;
}

double ComputeRho(const ValueFeatureProfile& profile, uint64_t sample_count) {
    if (!profile.is_t1b) return 1.0;
    if (sample_count == 0) return std::numeric_limits<double>::infinity();
    return std::sqrt(1.0 + profile.kappa_sample / static_cast<double>(sample_count));
}

bool PredictServiceableModel(const ValueSeriesRuntimeState& state,
                             const EventCalendarSpec* task_calendar,
                             int64_t bucket_id,
                             FormalPrediction* out_prediction,
                             std::shared_ptr<ValueFormalModel>* out_model,
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

SelectedValueBaseline ResolveServiceableBaseline(
    const std::string& key,
    int64_t bucket_id,
    const EventCalendarSpec* task_calendar,
    const std::unordered_map<std::string, ValueSeriesRuntimeState>& runtime_by_key,
    const std::unordered_map<std::string, BaselineSourceConfig>& series_override_map) {
    SelectedValueBaseline selected;

    // 基线来源的优先级固定为：先尝试本 key 的可服务模型，再按静态配置回退到来源 key。
    // 每一级内部都遵循相同规则：优先正式 formal，formal 不可用时才接受已训练但未切换的
    // candidate。这样热路径永远只消费“当前可预测”的模型，不把训练状态暴露给 task 层。
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
    return 1.0 - (1.0 - score_point) * (1.0 - kValueShiftWeight * score_shift);
}

double EffectiveValueSigma(double sigma_ref,
                           double rho_t,
                           double extra_scale = 1.0) {
    return std::max(kT1SigmaRefFloor, sigma_ref) * std::max(rho_t, 1.0) * extra_scale;
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

ValueDetectorCore::ValueDetectorCore(const ValueDetectorCoreSpec& spec)
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

int ValueDetectorCore::Submit(const ValueObservation& obs,
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

    const double x_t = TransformValueObservation(profile_, obs.value);
    const double rho_t = ComputeRho(profile_, obs.sample_count);
    const bool gate_score = !profile_.is_t1b || obs.sample_count >= profile_.n_score_min;
    const bool gate_shift = !profile_.is_t1b || obs.sample_count >= profile_.n_shift_min;
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
        double z_t = 0.0;
        double score_point = 0.0;
        double score_shift = 0.0;
        DriftDirection drift_direction = runtime_state.drift_state.direction;
        bool serviceable = false;
        bool shadow_active = false;
        BaselineProvider confidence_provider = BaselineProvider::kFormal;

        if (runtime_state.shadow_state.active && update.gap > kDriftConfig.g_reset) {
            runtime_state.shadow_state.Reset();
        }

        // `shadow baseline` 一旦激活，就优先复用冻结参考模型 + 单偏移量 delta。
        // 这条分支不再重新选来源，也不依赖历史数据；它的职责只是在线桥接
        // “旧基线已失配”到“新正式基线切换完成”之间的空档。
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
                const double mu_shadow =
                    shadow_prediction.value + runtime_state.shadow_state.delta;
                residual = x_t - mu_shadow;
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
                    const double sigma_shadow =
                        EffectiveValueSigma(shadow_prediction.sigma_ref,
                                            rho_t,
                                            kShadowSigmaScale);
                    z_t = residual / sigma_shadow;
                    out_submit->detector_result.raw_score = std::fabs(z_t);
                    score_point =
                        ComputePointScoreFromAbsResidual(out_submit->detector_result.raw_score);

                    runtime_state.shadow_state.delta =
                        (1.0 - kDriftConfig.alpha) * runtime_state.shadow_state.delta +
                        kDriftConfig.alpha * (x_t - shadow_prediction.value);
                }
                runtime_state.shadow_state.last_bucket_id = obs.bucket_id;
            } else {
                runtime_state.shadow_state.Reset();
            }
        }

        if (!serviceable) {
            // 常态路径先解析当前 bucket 应该使用哪个“可服务基线”。
            // 只有拿到可预测模型之后，才计算残差、漂移证据和点异常分数；
            // 冷启动或来源都不可服务时，只保留时序状态，不强行给出异常解释。
            const SelectedValueBaseline selected = ResolveServiceableBaseline(
                key, obs.bucket_id, TaskEventCalendar(spec_), runtime_by_key_, series_override_map_);
            runtime_state.baseline_source = selected.decision;
            if (selected.ready) {
                serviceable = true;
                runtime_state.model_state =
                    selected.decision.kind == BaselineSourceDecisionKind::kSelf
                        ? "serviceable_self"
                        : "serviceable_source";
                out_submit->detector_result.provider = selected.provider;
                confidence_provider = selected.provider;
                residual = x_t - selected.prediction.value;
                if (gate_score) {
                    const double sigma_eff =
                        EffectiveValueSigma(selected.prediction.sigma_ref, rho_t);
                    z_t = residual / sigma_eff;
                    out_submit->detector_result.raw_score = std::fabs(z_t);
                    score_point =
                        ComputePointScoreFromAbsResidual(out_submit->detector_result.raw_score);
                }

                const DriftUpdateResult drift_result = UpdateDriftState(
                    &runtime_state.drift_state,
                    kDriftConfig,
                    obs.bucket_id,
                    z_t,
                    gate_shift);
                runtime_state.last_p_shift = drift_result.p_shift;
                runtime_state.last_shift_confirmed = drift_result.shift_confirmed;
                drift_direction = drift_result.direction;
                score_shift =
                    ClipUnit((drift_result.p_shift - kDriftConfig.p_shift_low) /
                             (kDriftConfig.p_shift_high - kDriftConfig.p_shift_low));

                // 漂移确认后，不直接继续用旧 formal 硬扛，而是激活 shadow 并异步排队重建。
                // `rebuild_start_hint` 近似取连续确认段的起点，让慢路径优先回放新阶段数据。
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
                    runtime_state.shadow_state.delta = residual;
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

        runtime_state.last_sample_count = obs.sample_count;
        runtime_state.last_value = obs.value;
        runtime_state.last_x = x_t;
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

int ValueDetectorCore::BuildSeriesSnapshot(const BaselineStringRef& key,
                                           ValueSeriesSnapshot* out_snapshot) const {
    if (!out_snapshot) return error::BAD_REQUEST;

    SeriesState series_state;
    int rc = series_store_.GetState(key, &series_state);
    if (rc != error::OK) return rc;

    ValueSeriesRuntimeState runtime_state;
    {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        auto it = runtime_by_key_.find(CopyKey(key));
        if (it != runtime_by_key_.end()) runtime_state = it->second;
    }

    ValueSeriesSnapshot snapshot;
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

int ValueDetectorCore::GetSeriesState(const BaselineStringRef& key,
                                      SeriesState* out_state) const {
    return series_store_.GetState(key, out_state);
}

int ValueDetectorCore::BuildRebuildContext(const std::string& key,
                                           ValueRebuildContext* out_context) const {
    if (!out_context) return error::BAD_REQUEST;

    ValueRebuildContext context;
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

void ValueDetectorCore::ApplyFormalModel(
    const std::string& key,
    const ValueApplyFormalModelResult& apply_result) {
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
        // `replace_formal_model` 用于“新模型语义整体替换旧模型”的场景，
        // 尤其是 relation routed detector 在 basis 切换后，旧 formal 已经不再可比较。
        // 一旦正式切换成功，必须同步清空 shadow / drift / candidate 状态，
        // 防止旧阶段残留证据继续污染新 formal。
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

void ValueDetectorCore::MarkRebuildFailure(const DetectorRebuildFailure& failure) {
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

void ValueDetectorCore::ClearPendingRebuild(const std::string& key) {
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    auto it = runtime_by_key_.find(key);
    if (it == runtime_by_key_.end()) return;
    it->second.shift_rebuild_pending = false;
}

void ValueDetectorCore::Clear() {
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    runtime_by_key_.clear();
}

size_t ValueDetectorCore::Size() const {
    return series_store_.Size();
}

}  // namespace baseline
}  // namespace flowsql
