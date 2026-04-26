/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/detector/value_detector_core.h"

#include <common/error_code.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include "plugins/baseline/common/result_builder.h"
#include "plugins/baseline/model/profile_config.h"
#include "plugins/baseline/model/runtime_state_prune.h"
namespace flowsql {
namespace baseline {

namespace {

constexpr double kValueSigmaRefFloor = 1e-3;

const SharedProfileConfig& SharedConfig() {
    static const SharedProfileConfig config = DefaultSharedProfileConfig();
    return config;
}

std::string CopyKey(const BaselineStringRef& key) {
    if (!key.data || key.size == 0) return "";
    return std::string(key.data, key.size);
}

bool TryAdvancePruneBucket(std::atomic<int64_t>* last_pruned_bucket,
                           int64_t current_bucket) {
    if (!last_pruned_bucket || current_bucket < 0) return false;

    int64_t observed = last_pruned_bucket->load(std::memory_order_relaxed);
    while (current_bucket > observed) {
        if (last_pruned_bucket->compare_exchange_weak(observed,
                                                      current_bucket,
                                                      std::memory_order_relaxed,
                                                      std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void FillBadRequestResult(DetectorResult* out) {
    if (!out) return;
    *out = DetectorResult{};
    out->status = error::BAD_REQUEST;
}

void ResetCandidateSnapshot(FormalModelState* state) {
    if (!state) return;
    state->candidate_model_version = 0;
    state->candidate_model_kind = "none";
}

void BeginRebuildCycle(FormalModelState* state, RebuildSwitchState switch_state) {
    if (!state) return;
    ResetRebuildStageTrace(&state->stage_trace);
    ResetCandidateSnapshot(state);
    state->candidate_state = RebuildCandidateState::kNone;
    state->switch_state = switch_state;
    state->failure_reason = RebuildFailureReason::kNone;
    state->failure_reason_detail.clear();
}

void UpdateCandidateSnapshot(FormalModelState* state,
                             uint64_t candidate_model_version,
                             const char* candidate_model_kind) {
    if (!state) return;
    state->candidate_generation = candidate_model_version;
    state->candidate_model_version = candidate_model_version;
    state->candidate_model_kind = candidate_model_kind ? candidate_model_kind : "none";
}

void ApplyRebuildOutcome(FormalModelState* state,
                         const ValueApplyFormalModelResult& apply_result) {
    if (!state) return;
    state->candidate_state = apply_result.candidate_state;
    state->switch_state = apply_result.switch_state;
    state->failure_reason = apply_result.failure_reason;
    state->failure_reason_detail = apply_result.failure_reason_detail;
}

struct SelectedValueBaseline {
    bool ready = false;
    BaselineProvider provider = BaselineProvider::kFormal;
    BaselineSourceDecision decision;
    FormalPrediction prediction;
    ShadowRefKind shadow_ref_kind = ShadowRefKind::kNone;
    std::shared_ptr<ValueFormalModel> model;
};

struct ValueSourceRuntimeView {
    std::string key;
    ValueSeriesRuntimeState runtime_state;
};

BaselineTaskSpec BuildPredictTaskSpec(const ValueDetectorCoreSpec& spec,
                                      const std::string& series_key) {
    BaselineTaskSpec task_spec;
    task_spec.key = series_key;
    task_spec.feature = spec.routed_feature_id;
    task_spec.delta = spec.delta;
    task_spec.tz = spec.tz;
    return task_spec;
}

int PredictValueModel(const ValueDetectorCoreSpec& spec,
                      const std::string& series_key,
                      const ValueFormalModel* model,
                      int64_t bucket_id,
                      FormalPrediction* out_prediction) {
    BaselineTaskSpec task_spec = BuildPredictTaskSpec(spec, series_key);
    FormalPredictContext context;
    context.task_spec = &task_spec;
    context.event_calendar = spec.compiled_event_calendar.get();
    context.bucket_id = bucket_id;
    return PredictFormalModel(model, context, out_prediction);
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

    if (spec.feature_type == "value_basic") {
        profile.is_sampled = false;
        if (spec.transform_kind.has_value()) {
            if (*spec.transform_kind != "identity" && *spec.transform_kind != "log1p") {
                if (err) *err = "unsupported value_basic transform_kind";
                return false;
            }
            profile.transform_name = *spec.transform_kind;
        }
    } else if (spec.feature_type == "value_sampled") {
        ValueSampledProfileConfig profile_config;
        if (!GetValueSampledProfileConfig(spec.feature_profile, &profile_config)) {
            if (err) *err = "unsupported value_sampled feature_profile";
            return false;
        }
        profile.is_sampled = true;
        profile.transform_name = profile_config.transform_name_override;
        profile.n_train_min = profile_config.n_train_min;
        profile.n_score_min = profile_config.n_score_min();
        profile.n_shift_min = profile_config.n_shift_min();
        profile.kappa_sample = profile_config.kappa_sample();
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
    if (profile.is_sampled && obs.sample_count == 0) {
        if (err) *err = "sample_count must be >= 1 for value_sampled";
        return error::BAD_REQUEST;
    }
    return error::OK;
}

double ComputeRho(const ValueFeatureProfile& profile, uint64_t sample_count) {
    if (!profile.is_sampled) return 1.0;
    if (sample_count == 0) return std::numeric_limits<double>::infinity();
    return std::sqrt(1.0 + profile.kappa_sample / static_cast<double>(sample_count));
}

bool PredictServiceableModel(const ValueDetectorCoreSpec& spec,
                             const std::string& series_key,
                             const ValueSeriesRuntimeState& state,
                             int64_t bucket_id,
                             FormalPrediction* out_prediction,
                             std::shared_ptr<ValueFormalModel>* out_model,
                             ShadowRefKind* out_ref_kind,
                             bool source_kind) {
    FormalPrediction prediction;
    if (state.formal_state.formal_ready &&
        PredictValueModel(spec, series_key, state.formal_model.get(), bucket_id, &prediction) ==
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
    return false;
}

SelectedValueBaseline ResolveServiceableBaseline(
    const ValueDetectorCoreSpec& spec,
    const std::string& key,
    int64_t bucket_id,
    const ValueSeriesRuntimeState& self_runtime_state,
    const std::vector<ValueSourceRuntimeView>& source_runtime_states,
    const BaselineSourceConfig* baseline_source_config) {
    SelectedValueBaseline selected;

    // 数值特征热路径只允许 formal source 提供在线解释：
    // 先尝试 self formal，再按静态配置回退到 configured source formal。
    // candidate model 仅保留给慢路径验证与快照观测，不参与正式在线服务。
    if (PredictServiceableModel(spec,
                                key,
                                self_runtime_state,
                                bucket_id,
                                &selected.prediction,
                                &selected.model,
                                &selected.shadow_ref_kind,
                                false)) {
        selected.ready = true;
        selected.provider = BaselineProvider::kFormal;
        selected.decision.selected_kind = BaselineSourceKind::kSelf;
        selected.decision.serviceable = true;
        return selected;
    }

    if (!baseline_source_config) return selected;

    for (const auto& source_ref : baseline_source_config->sources) {
        auto source_it = std::find_if(source_runtime_states.begin(),
                                      source_runtime_states.end(),
                                      [&source_ref](const ValueSourceRuntimeView& view) {
                                          return view.key == source_ref.source_key;
                                      });
        if (source_it == source_runtime_states.end()) continue;
        if (!PredictServiceableModel(spec,
                                     source_it->key,
                                     source_it->runtime_state,
                                     bucket_id,
                                     &selected.prediction,
                                     &selected.model,
                                     &selected.shadow_ref_kind,
                                     true)) {
            continue;
        }

        selected.ready = true;
        selected.provider = BaselineProvider::kSource;
        selected.decision.selected_kind = BaselineSourceKind::kConfiguredSource;
        selected.decision.selected_source_key = source_ref.source_key;
        selected.decision.serviceable = true;
        return selected;
    }

    return selected;
}

double ComputePointScoreFromAbsResidual(double abs_residual) {
    const SharedProfileConfig& config = SharedConfig();
    if (abs_residual <= config.z_warn) return 0.0;
    return ClipUnit((abs_residual - config.z_warn) / (config.z_crit - config.z_warn));
}

double ComputeNormalizedScore(double score_point, double score_shift) {
    return 1.0 - (1.0 - score_point) * (1.0 - SharedConfig().w_shift * score_shift);
}

double EffectiveValueSigma(double sigma_ref,
                           double rho_t,
                           double extra_scale = 1.0) {
    return std::max(kValueSigmaRefFloor, sigma_ref) * std::max(rho_t, 1.0) * extra_scale;
}

BaselineStringRef StringRefOf(const std::string& value) {
    if (value.empty()) return BaselineStringRef{};
    return BaselineStringRef{value.c_str(), static_cast<uint32_t>(value.size())};
}

void FillResultIdentity(const ValueDetectorCoreSpec& spec,
                        const ValueFeatureProfile& profile,
                        const ValueObservation& obs,
                        DetectorResult* out) {
    if (!out) return;
    out->key = obs.key;
    out->feature = StringRefOf(spec.routed_feature_id);
    out->feature_type = StringRefOf(profile.feature_type);
    out->ts = obs.bucket_id;
}

BaselineSourceKind SourceKindFromShadowRef(ShadowRefKind kind) {
    return ShadowRefUsesSource(kind) ? BaselineSourceKind::kConfiguredSource
                                     : BaselineSourceKind::kSelf;
}

BaselineModelState EvidenceModelState(const std::string& model_state) {
    if (model_state == "cold_start") return BaselineModelState::kColdStart;
    if (model_state == "shadow_self" || model_state == "shadow_source") {
        return BaselineModelState::kShadow;
    }
    if (model_state == "serviceable_source") {
        return BaselineModelState::kConfiguredSource;
    }
    if (model_state == "serviceable_self") {
        return BaselineModelState::kFormal;
    }
    return BaselineModelState::kUnknown;
}

double DriftDirectionSign(DriftDirection direction) {
    switch (direction) {
        case DriftDirection::kUp:
            return 1.0;
        case DriftDirection::kDown:
            return -1.0;
        case DriftDirection::kNone:
            break;
    }
    return 0.0;
}

void FillValueEvidence(DetectorResult* out,
                       const ValueFeatureProfile& profile,
                       const ValueSeriesRuntimeState& runtime_state,
                       double y_t,
                       double x_t,
                       double baseline_mu_t,
                       double residual,
                       double z_t,
                       double score_point,
                       double score_shift,
                       uint64_t sample_count,
                       double sigma_eff_t,
                       bool shadow_active) {
    if (!out) return;

    out->evidence.kind = BaselineEvidenceKind::kValue;
    out->evidence.value = ValueEvidence{};

    ValueEvidence& evidence = out->evidence.value;
    evidence.y_t = y_t;
    evidence.x_t = x_t;
    evidence.baseline_mu_t = baseline_mu_t;
    evidence.resid_r_t = residual;
    evidence.z_t = z_t;
    evidence.p_shift_t = runtime_state.last_p_shift;
    evidence.dir_t = DriftDirectionSign(runtime_state.drift_state.direction);
    evidence.score_point = score_point;
    evidence.score_shift = score_shift;
    evidence.baseline_source_kind = runtime_state.baseline_source.selected_kind;
    evidence.model_state = EvidenceModelState(runtime_state.model_state);
    evidence.shadow_active = shadow_active;

    if (profile.is_sampled) {
        evidence.field_flags |= kBaselineEvidenceHasSampleCount;
        evidence.field_flags |= kBaselineEvidenceHasSigmaEff;
        evidence.sample_count = sample_count;
        evidence.sigma_eff_t = sigma_eff_t;
    }

    if (runtime_state.baseline_source.selected_kind == BaselineSourceKind::kConfiguredSource &&
        !runtime_state.baseline_source.selected_source_key.empty()) {
        evidence.field_flags |= kBaselineEvidenceHasSourceKey;
        evidence.baseline_source_key =
            StringRefOf(runtime_state.baseline_source.selected_source_key);
    }
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

    // 来源配置的真实生效粒度是 `(key, feature)`。本 core 的 feature 固定，
    // 因此热路径只需要按运行时 key 做一次 O(1) 查找。
    for (const auto& source_config : spec_.baseline_source_configs) {
        baseline_source_config_by_key_.emplace(source_config.key, source_config.config);
    }
}

int ValueDetectorCore::Submit(const ValueObservation& obs,
                              DetectorSubmitOutput* out_submit) {
    if (!out_submit) return error::BAD_REQUEST;
    *out_submit = DetectorSubmitOutput{};
    out_submit->rebuild_intent.routed_feature_id = spec_.routed_feature_id;
    FillResultIdentity(spec_, profile_, obs, &out_submit->detector_result);

    std::string err;
    int rc = ValidateObservation(profile_, obs, &err);
    if (rc != error::OK) {
        FillBadRequestResult(&out_submit->detector_result);
        FillResultIdentity(spec_, profile_, obs, &out_submit->detector_result);
        return rc;
    }

    const double x_t = TransformValueObservation(profile_, obs.value);
    const double rho_t = ComputeRho(profile_, obs.sample_count);
    const bool gate_score = !profile_.is_sampled || obs.sample_count >= profile_.n_score_min;
    const bool gate_shift = !profile_.is_sampled || obs.sample_count >= profile_.n_shift_min;
    const std::string key = CopyKey(obs.key);
    const BaselineSourceConfig* baseline_source_config = nullptr;
    auto source_config_it = baseline_source_config_by_key_.find(key);
    if (source_config_it != baseline_source_config_by_key_.end()) {
        baseline_source_config = &source_config_it->second;
    }

    bool enqueue_rebuild = false;
    int64_t rebuild_start_hint = 0;
    {
        std::vector<size_t> shard_ids;
        shard_ids.reserve(1 + (baseline_source_config ? baseline_source_config->sources.size() : 0));
        shard_ids.push_back(RuntimeShardIndex(key));
        if (baseline_source_config) {
            for (const auto& source_ref : baseline_source_config->sources) {
                shard_ids.push_back(RuntimeShardIndex(source_ref.source_key));
            }
        }
        std::sort(shard_ids.begin(), shard_ids.end());
        shard_ids.erase(std::unique(shard_ids.begin(), shard_ids.end()), shard_ids.end());

        std::vector<std::unique_lock<std::mutex>> shard_locks;
        shard_locks.reserve(shard_ids.size());
        for (size_t shard_id : shard_ids) {
            shard_locks.emplace_back(runtime_shards_[shard_id].mutex);
        }

        RuntimeShardState& self_shard = runtime_shards_[RuntimeShardIndex(key)];
        auto& entry = self_shard.states[key];
        SeriesUpdateResult update =
            entry.series_state.ApplyObservation(obs.bucket_id, SeriesPersistenceMode::kFreeze, false);
        if (update.status != error::OK) {
            FillBaseResult(update, &out_submit->detector_result);
            out_submit->detector_result.status = update.status;
            return update.status;
        }

        FillBaseResult(update, &out_submit->detector_result);
        out_submit->detector_result.status = error::OK;
        out_submit->detector_result.raw_score = 0.0;
        out_submit->detector_result.normalized_score = 0.0;
        out_submit->detector_result.confidence = 0.0;
        out_submit->detector_result.direction = BaselineDirection::kUnknown;
        out_submit->detector_result.severity = BaselineSeverity::kInfo;
        out_submit->detector_result.provider = BaselineProvider::kNone;
        out_submit->detector_result.reason = BaselineReasonCode::kUnknown;

        auto& runtime_state = entry.runtime_state;
        UpdateCoverageStats(&runtime_state.readiness_state, obs.bucket_id, true);
        double residual = 0.0;
        double baseline_mu_t = 0.0;
        double z_t = 0.0;
        double sigma_eff_t = 0.0;
        double score_point = 0.0;
        double score_shift = 0.0;
        DriftDirection drift_direction = runtime_state.drift_state.direction;
        bool serviceable = false;
        bool shadow_active = false;

        const SharedProfileConfig& shared_config = SharedConfig();
        const DriftConfig& drift_config = shared_config.drift;

        if (runtime_state.shadow_state.active && update.gap > drift_config.g_reset) {
            runtime_state.shadow_state.Reset();
        }

        // `shadow baseline` 一旦激活，就优先复用冻结参考模型 + 单偏移量 delta。
        // 这条分支不再重新选来源，也不依赖历史数据；它的职责只是在线桥接
        // “旧基线已失配”到“新正式基线切换完成”之间的空档。
        if (runtime_state.shadow_state.active &&
            runtime_state.shadow_state.frozen_ref_model) {
            FormalPrediction shadow_prediction;
            const std::string shadow_key =
                ShadowRefUsesSource(runtime_state.shadow_state.ref_kind) &&
                        !runtime_state.shadow_state.ref_source_key.empty()
                    ? runtime_state.shadow_state.ref_source_key
                    : key;
            if (PredictValueModel(spec_,
                                  shadow_key,
                                  runtime_state.shadow_state.frozen_ref_model.get(),
                                  obs.bucket_id,
                                  &shadow_prediction) == error::OK &&
                shadow_prediction.ready) {
                serviceable = true;
                shadow_active = true;
                const double mu_shadow =
                    shadow_prediction.value + runtime_state.shadow_state.delta;
                baseline_mu_t = mu_shadow;
                residual = x_t - mu_shadow;
                runtime_state.model_state =
                    ShadowRefUsesSource(runtime_state.shadow_state.ref_kind)
                        ? "shadow_source"
                        : "shadow_self";
                runtime_state.baseline_source.selected_kind =
                    ShadowRefUsesSource(runtime_state.shadow_state.ref_kind)
                        ? BaselineSourceKind::kConfiguredSource
                        : BaselineSourceKind::kSelf;
                runtime_state.baseline_source.selected_source_key =
                    runtime_state.shadow_state.ref_source_key;
                runtime_state.baseline_source.serviceable = true;
                out_submit->detector_result.provider = BaselineProvider::kShadow;
                out_submit->detector_result.flags |= kBaselineFlagShadowActive;
                const BaselineSourceKind shadow_source_kind =
                    SourceKindFromShadowRef(runtime_state.shadow_state.ref_kind);
                RefreshOnlineReadiness(&runtime_state.readiness_state,
                                       shared_config,
                                       shadow_prediction.readiness,
                                       shadow_source_kind);

                if (gate_score) {
                    const double sigma_shadow =
                        EffectiveValueSigma(shadow_prediction.sigma_ref,
                                            rho_t,
                                            ValueShadowSigmaScale());
                    sigma_eff_t = sigma_shadow;
                    z_t = residual / sigma_shadow;
                    out_submit->detector_result.raw_score = std::fabs(z_t);
                    score_point =
                        ComputePointScoreFromAbsResidual(out_submit->detector_result.raw_score);

                    runtime_state.shadow_state.delta =
                        (1.0 - drift_config.alpha) * runtime_state.shadow_state.delta +
                        drift_config.alpha * (x_t - shadow_prediction.value);
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
            std::vector<ValueSourceRuntimeView> source_runtime_states;
            if (baseline_source_config) {
                source_runtime_states.reserve(baseline_source_config->sources.size());
                for (const auto& source_ref : baseline_source_config->sources) {
                    const RuntimeShardState& source_shard =
                        runtime_shards_[RuntimeShardIndex(source_ref.source_key)];
                    auto source_it = source_shard.states.find(source_ref.source_key);
                    if (source_it == source_shard.states.end()) continue;
                    source_runtime_states.push_back(
                        ValueSourceRuntimeView{source_ref.source_key, source_it->second.runtime_state});
                }
            }
            const SelectedValueBaseline selected = ResolveServiceableBaseline(
                spec_,
                key,
                obs.bucket_id,
                runtime_state,
                source_runtime_states,
                baseline_source_config);
            runtime_state.baseline_source = selected.decision;
            if (selected.ready) {
                serviceable = true;
                runtime_state.model_state =
                    selected.decision.selected_kind == BaselineSourceKind::kSelf
                        ? "serviceable_self"
                        : "serviceable_source";
                out_submit->detector_result.provider = selected.provider;
                baseline_mu_t = selected.prediction.value;
                residual = x_t - selected.prediction.value;
                RefreshOnlineReadiness(&runtime_state.readiness_state,
                                       shared_config,
                                       selected.prediction.readiness,
                                       selected.decision.selected_kind);
                if (gate_score) {
                    sigma_eff_t = EffectiveValueSigma(selected.prediction.sigma_ref, rho_t);
                    z_t = residual / sigma_eff_t;
                    out_submit->detector_result.raw_score = std::fabs(z_t);
                    score_point =
                        ComputePointScoreFromAbsResidual(out_submit->detector_result.raw_score);
                }

                const DriftUpdateResult drift_result = UpdateDriftState(
                    &runtime_state.drift_state,
                    drift_config,
                    obs.bucket_id,
                    z_t,
                    gate_shift);
                runtime_state.last_p_shift = drift_result.p_shift;
                runtime_state.last_shift_confirmed = drift_result.shift_confirmed;
                drift_direction = drift_result.direction;
                score_shift =
                    ClipUnit((drift_result.p_shift - drift_config.p_shift_low) /
                             (drift_config.p_shift_high - drift_config.p_shift_low));

                // 漂移确认后，不直接继续用旧 formal 硬扛，而是激活 shadow 并异步排队重建。
                // `rebuild_start_hint` 近似取连续确认段的起点，让慢路径优先回放新阶段数据。
                if (drift_result.shift_confirmed &&
                    runtime_state.drift_state.confirm_count >= drift_config.m_shift &&
                    !runtime_state.shadow_state.active &&
                    !runtime_state.shift_rebuild_pending &&
                    selected.model) {
                    runtime_state.shadow_state.active = true;
                    runtime_state.shadow_state.ref_kind = selected.shadow_ref_kind;
                    runtime_state.shadow_state.ref_source_key =
                        selected.decision.selected_kind ==
                                BaselineSourceKind::kConfiguredSource
                            ? selected.decision.selected_source_key
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
                    runtime_state.formal_state.switch_state =
                        RebuildSwitchState::kShadowActive;
                    runtime_state.formal_state.failure_reason =
                        RebuildFailureReason::kNone;
                    runtime_state.formal_state.failure_reason_detail.clear();
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
                }
            } else {
                runtime_state.model_state = "cold_start";
                runtime_state.baseline_source = BaselineSourceDecision{};
                runtime_state.last_p_shift = 0.0;
                runtime_state.last_shift_confirmed = false;
                RefreshOnlineReadiness(&runtime_state.readiness_state,
                                       shared_config,
                                       ModelReadiness::kNotReady,
                                       BaselineSourceKind::kNone);
                out_submit->detector_result.provider = BaselineProvider::kNone;
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

            double confidence_base = runtime_state.readiness_state.confidence_base;
            if (shadow_active) {
                confidence_base = std::min(confidence_base, ValueShadowConfidenceCap());
            }
            out_submit->detector_result.confidence =
                confidence_base / std::max(rho_t, 1.0);

            FillValueEvidence(&out_submit->detector_result,
                              profile_,
                              runtime_state,
                              obs.value,
                              x_t,
                              baseline_mu_t,
                              residual,
                              z_t,
                              score_point,
                              score_shift,
                              obs.sample_count,
                              sigma_eff_t,
                              shadow_active);
        }

        runtime_state.last_sample_count = obs.sample_count;
        runtime_state.last_value = obs.value;
        runtime_state.last_x = x_t;
        runtime_state.last_rho = rho_t;
        runtime_state.last_gate_score = gate_score;
        runtime_state.last_gate_shift = gate_shift;
    }

    if (TryAdvancePruneBucket(&last_pruned_bucket_, obs.bucket_id)) {
        RuntimeShardState& prune_shard =
            runtime_shards_[prune_cursor_.fetch_add(1, std::memory_order_relaxed) % kShardCount];
        std::lock_guard<std::mutex> prune_lock(prune_shard.mutex);
        pruned_key_count_total_.fetch_add(
            PruneBoundedStateMap(&prune_shard.states,
                                 &prune_shard.prune_cursor,
                                 RuntimeIdlePruneScanLimit(),
                                 [bucket_id = obs.bucket_id](const ValueSeriesShardEntry& entry) {
                                     return RuntimeStateIdleBeyondGap(
                                         entry.series_state.last_bucket_id, bucket_id);
                                 }),
            std::memory_order_relaxed);
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

    const std::string key_copy = CopyKey(key);
    if (key_copy.empty()) return error::BAD_REQUEST;

    ValueSeriesShardEntry entry;
    const RuntimeShardState& shard = runtime_shards_[RuntimeShardIndex(key_copy)];
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto it = shard.states.find(key_copy);
        if (it == shard.states.end() || !it->second.series_state.initialized) {
            return error::NOT_FOUND;
        }
        entry = it->second;
    }

    ValueSeriesSnapshot snapshot;
    snapshot.series_state = entry.series_state;
    snapshot.runtime_state = entry.runtime_state;
    const ValueSeriesRuntimeState& runtime_state = snapshot.runtime_state;
    const std::string& series_key = key_copy;
    snapshot.formal_predict_status = PredictValueModel(
        spec_,
        series_key,
        runtime_state.formal_model.get(),
        snapshot.series_state.last_bucket_id,
        &snapshot.formal_prediction);
    snapshot.candidate_predict_status = PredictValueModel(
        spec_,
        series_key,
        runtime_state.candidate_model.get(),
        snapshot.series_state.last_bucket_id,
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
    if (!out_state) return error::BAD_REQUEST;

    const std::string key_copy = CopyKey(key);
    if (key_copy.empty()) return error::BAD_REQUEST;

    const RuntimeShardState& shard = runtime_shards_[RuntimeShardIndex(key_copy)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.states.find(key_copy);
    if (it == shard.states.end() || !it->second.series_state.initialized) {
        return error::NOT_FOUND;
    }
    *out_state = it->second.series_state;
    return error::OK;
}

int ValueDetectorCore::BuildRebuildContext(const std::string& key,
                                           ValueRebuildContext* out_context) const {
    if (!out_context) return error::BAD_REQUEST;

    ValueRebuildContext context;
    {
        const RuntimeShardState& shard = runtime_shards_[RuntimeShardIndex(key)];
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto it = shard.states.find(key);
        if (it != shard.states.end()) {
            context.next_model_version =
                it->second.runtime_state.formal_state.candidate_generation + 1;
            context.incumbent_formal_model = it->second.runtime_state.formal_model;
            context.incumbent_shadow_state = it->second.runtime_state.shadow_state;
        }
    }

    *out_context = std::move(context);
    return error::OK;
}

void ValueDetectorCore::MarkRebuildEnqueued(const std::string& key) {
    RuntimeShardState& shard = runtime_shards_[RuntimeShardIndex(key)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto& runtime_state = shard.states[key].runtime_state;
    runtime_state.shift_rebuild_pending = true;
    BeginRebuildCycle(&runtime_state.formal_state, RebuildSwitchState::kRebuildPending);
    runtime_state.formal_state.candidate_state = RebuildCandidateState::kBuilding;
    MarkRebuildStageBuilding(&runtime_state.formal_state.stage_trace);
}

void ValueDetectorCore::MarkCandidateBuilding(const std::string& key) {
    RuntimeShardState& shard = runtime_shards_[RuntimeShardIndex(key)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto& runtime_state = shard.states[key].runtime_state;
    runtime_state.shift_rebuild_pending = true;
    runtime_state.formal_state.candidate_state = RebuildCandidateState::kBuilding;
    runtime_state.formal_state.switch_state = RebuildSwitchState::kRebuildPending;
    runtime_state.formal_state.failure_reason = RebuildFailureReason::kNone;
    runtime_state.formal_state.failure_reason_detail.clear();
    ResetCandidateSnapshot(&runtime_state.formal_state);
    MarkRebuildStageBuilding(&runtime_state.formal_state.stage_trace);
}

void ValueDetectorCore::MarkCandidateBuilt(const std::string& key,
                                           uint64_t candidate_model_version,
                                           const char* candidate_model_kind) {
    RuntimeShardState& shard = runtime_shards_[RuntimeShardIndex(key)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto& runtime_state = shard.states[key].runtime_state;
    runtime_state.formal_state.candidate_state = RebuildCandidateState::kBuilt;
    runtime_state.formal_state.switch_state = RebuildSwitchState::kRebuildPending;
    runtime_state.formal_state.failure_reason = RebuildFailureReason::kNone;
    runtime_state.formal_state.failure_reason_detail.clear();
    UpdateCandidateSnapshot(&runtime_state.formal_state,
                            candidate_model_version,
                            candidate_model_kind);
    MarkRebuildStageBuilt(&runtime_state.formal_state.stage_trace);
}

void ValueDetectorCore::MarkCandidateValidating(const std::string& key,
                                                uint64_t candidate_model_version,
                                                const char* candidate_model_kind) {
    RuntimeShardState& shard = runtime_shards_[RuntimeShardIndex(key)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto& runtime_state = shard.states[key].runtime_state;
    runtime_state.formal_state.candidate_state = RebuildCandidateState::kValidating;
    runtime_state.formal_state.switch_state = RebuildSwitchState::kValidating;
    runtime_state.formal_state.failure_reason = RebuildFailureReason::kNone;
    runtime_state.formal_state.failure_reason_detail.clear();
    UpdateCandidateSnapshot(&runtime_state.formal_state,
                            candidate_model_version,
                            candidate_model_kind);
    MarkRebuildStageValidating(&runtime_state.formal_state.stage_trace);
}

void ValueDetectorCore::ApplyFormalModel(
    const std::string& key,
    const ValueApplyFormalModelResult& apply_result) {
    RuntimeShardState& shard = runtime_shards_[RuntimeShardIndex(key)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto& runtime_state = shard.states[key].runtime_state;
    runtime_state.formal_state.last_replay_window = apply_result.replay_window;
    runtime_state.formal_state.last_train_window = apply_result.train_window;
    runtime_state.formal_state.last_holdout_window = apply_result.holdout_window;
    runtime_state.formal_state.last_candidate_loss = apply_result.candidate_loss;
    runtime_state.formal_state.last_incumbent_loss = apply_result.incumbent_loss;
    runtime_state.formal_state.last_validation_count = apply_result.validation_count;
    runtime_state.shift_rebuild_pending = false;
    ApplyRebuildOutcome(&runtime_state.formal_state, apply_result);
    ResetCandidateSnapshot(&runtime_state.formal_state);

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
            runtime_state.candidate_replay.reset();
            runtime_state.candidate_model.reset();
            runtime_state.shadow_state.Reset();
            runtime_state.drift_state.Reset();
            runtime_state.last_p_shift = 0.0;
            runtime_state.last_shift_confirmed = false;
        } else {
            runtime_state.candidate_replay.reset();
            runtime_state.candidate_model.reset();
        }
    } else {
        runtime_state.candidate_replay.reset();
        runtime_state.candidate_model.reset();
    }
}

void ValueDetectorCore::MarkRebuildFailure(const DetectorRebuildFailure& failure) {
    RuntimeShardState& shard = runtime_shards_[RuntimeShardIndex(failure.key)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto& runtime_state = shard.states[failure.key].runtime_state;
    runtime_state.formal_state.candidate_state = failure.candidate_state;
    runtime_state.formal_state.switch_state = failure.switch_state;
    runtime_state.formal_state.failure_reason = failure.failure_reason;
    runtime_state.formal_state.failure_reason_detail = failure.failure_reason_detail;
    ResetCandidateSnapshot(&runtime_state.formal_state);
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
    RuntimeShardState& shard = runtime_shards_[RuntimeShardIndex(key)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.states.find(key);
    if (it == shard.states.end()) return;
    it->second.runtime_state.shift_rebuild_pending = false;
    FormalModelState& formal_state = it->second.runtime_state.formal_state;
    formal_state.candidate_state = RebuildCandidateState::kNone;
    formal_state.failure_reason = RebuildFailureReason::kNone;
    formal_state.failure_reason_detail.clear();
    formal_state.switch_state =
        it->second.runtime_state.shadow_state.active
            ? RebuildSwitchState::kShadowActive
            : RebuildSwitchState::kIdle;
    ResetRebuildStageTrace(&formal_state.stage_trace);
    ResetCandidateSnapshot(&formal_state);
}

void ValueDetectorCore::Clear() {
    for (auto& shard : runtime_shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.states.clear();
        shard.prune_cursor = 0;
    }
    prune_cursor_.store(0, std::memory_order_relaxed);
    last_pruned_bucket_.store(-1, std::memory_order_relaxed);
    pruned_key_count_total_.store(0, std::memory_order_relaxed);
}

size_t ValueDetectorCore::Size() const {
    size_t total = 0;
    for (const auto& shard : runtime_shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (const auto& entry : shard.states) {
            if (entry.second.series_state.initialized) ++total;
        }
    }
    return total;
}

uint64_t ValueDetectorCore::PrunedKeyCount() const {
    return pruned_key_count_total_.load(std::memory_order_relaxed);
}

int64_t ValueDetectorCore::IdlePruneBucketGap() const {
    return RuntimeIdlePruneBucketGap();
}

}  // namespace baseline
}  // namespace flowsql
