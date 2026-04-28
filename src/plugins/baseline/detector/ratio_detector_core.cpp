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
#include "plugins/baseline/config/runtime_config.h"
#include "plugins/baseline/model/profile_config.h"
#include "plugins/baseline/model/runtime_state_prune.h"

namespace flowsql {
namespace baseline {

namespace {

constexpr double kSlowDriftVarFloor = 1e-12;

SharedProfileConfig SharedConfig() {
    return DefaultSharedProfileConfig();
}

void ResetSlowDriftTracking(RatioShadowState* shadow_state) {
    if (!shadow_state) return;
    shadow_state->slow_err_ring.clear();
    shadow_state->slow_var_ring.clear();
    shadow_state->slow_ring_pos = 0;
    shadow_state->slow_ring_size = 0;
    shadow_state->slow_err_sum = 0.0;
    shadow_state->slow_var_sum = 0.0;
    shadow_state->z_win = 0.0;
    shadow_state->slow_drift_triggered = false;
}

void EnsureSlowDriftWindow(RatioShadowState* shadow_state, std::size_t window_size) {
    if (!shadow_state) return;
    if (window_size == 0) {
        ResetSlowDriftTracking(shadow_state);
        return;
    }
    if (shadow_state->slow_err_ring.size() == window_size &&
        shadow_state->slow_var_ring.size() == window_size) {
        return;
    }
    ResetSlowDriftTracking(shadow_state);
    shadow_state->slow_err_ring.assign(window_size, 0.0);
    shadow_state->slow_var_ring.assign(window_size, 0.0);
}

void UpdateSlowDriftTracking(RatioShadowState* shadow_state,
                             std::size_t window_size,
                             double residual,
                             double sigma) {
    if (!shadow_state) return;
    EnsureSlowDriftWindow(shadow_state, window_size);
    if (window_size == 0 || shadow_state->slow_err_ring.empty() ||
        shadow_state->slow_var_ring.empty()) {
        return;
    }
    const std::size_t idx = shadow_state->slow_ring_pos;
    const bool buffer_full = shadow_state->slow_ring_size >= window_size;
    if (buffer_full) {
        shadow_state->slow_err_sum -= shadow_state->slow_err_ring[idx];
        shadow_state->slow_var_sum -= shadow_state->slow_var_ring[idx];
    } else {
        ++shadow_state->slow_ring_size;
    }

    const double sigma_sq = sigma * sigma;
    shadow_state->slow_err_ring[idx] = residual;
    shadow_state->slow_var_ring[idx] = sigma_sq;
    shadow_state->slow_err_sum += residual;
    shadow_state->slow_var_sum += sigma_sq;
    shadow_state->slow_ring_pos = (idx + 1) % window_size;

    if (shadow_state->slow_ring_size >= window_size) {
        const double denom = std::sqrt(std::max(shadow_state->slow_var_sum, kSlowDriftVarFloor));
        shadow_state->z_win = shadow_state->slow_err_sum / denom;
        shadow_state->slow_drift_triggered =
            std::fabs(shadow_state->z_win) >= ShadowZWinShiftThreshold();
    } else {
        shadow_state->z_win = 0.0;
        shadow_state->slow_drift_triggered = false;
    }
}

bool IsCandidateRunning(RebuildCandidateState state) {
    return state == RebuildCandidateState::kBuilding ||
           state == RebuildCandidateState::kBuilt ||
           state == RebuildCandidateState::kValidating;
}

void NormalizeSwitchState(FormalModelState* state, bool shadow_active) {
    if (!state) return;
    if (state->switch_state == RebuildSwitchState::kFormalApplied) return;
    if (IsCandidateRunning(state->candidate_state)) {
        state->switch_state = state->candidate_state == RebuildCandidateState::kValidating
                                  ? RebuildSwitchState::kValidating
                                  : RebuildSwitchState::kRebuildPending;
        return;
    }
    state->switch_state =
        shadow_active ? RebuildSwitchState::kShadowActive : RebuildSwitchState::kIdle;
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
    state->candidate_passed = false;
    state->full_waiting = false;
    state->t_switch_gate = -1;
    state->t_full_start = -1;
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
                         const RatioApplyFormalModelResult& apply_result) {
    if (!state) return;
    state->candidate_state = apply_result.candidate_state;
    state->switch_state = apply_result.switch_state;
    state->failure_reason = apply_result.failure_reason;
    state->failure_reason_detail = apply_result.failure_reason_detail;
    state->candidate_passed = apply_result.candidate_passed;
    state->full_waiting = apply_result.full_waiting;
    state->t_switch_gate = apply_result.t_switch_gate;
    state->t_full_start = apply_result.t_full_start;
}

struct SelectedRatioBaseline {
    bool ready = false;
    BaselineProvider provider = BaselineProvider::kNone;
    BaselineSourceDecision decision;
    FormalPrediction prediction;
    ShadowRefKind shadow_ref_kind = ShadowRefKind::kNone;
    std::shared_ptr<RatioFormalModel> model;
};

struct RatioSourceRuntimeView {
    std::string key;
    RatioSeriesRuntimeState runtime_state;
};

BaselineTaskSpec BuildPredictTaskSpec(const RatioDetectorCoreSpec& spec,
                                      const std::string& series_key) {
    BaselineTaskSpec task_spec;
    task_spec.key = series_key;
    task_spec.feature = spec.routed_feature_id;
    task_spec.delta = spec.delta;
    task_spec.tz = spec.tz;
    return task_spec;
}

int PredictRatioModel(const RatioDetectorCoreSpec& spec,
                      const std::string& series_key,
                      const RatioFormalModel* model,
                      int64_t bucket_id,
                      FormalPrediction* out_prediction) {
    BaselineTaskSpec task_spec = BuildPredictTaskSpec(spec, series_key);
    FormalPredictContext context;
    context.task_spec = &task_spec;
    context.event_calendar = spec.compiled_event_calendar.get();
    context.bucket_id = bucket_id;
    return PredictFormalModel(model, context, out_prediction);
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

    RatioProfileConfig profile_config;
    if (!GetRatioProfileConfig(spec.feature_profile, &profile_config)) {
        if (err) *err = "unsupported ratio feature_profile";
        return false;
    }

    profile.d_min_train = profile_config.d_min_train;
    profile.d_score_min = profile_config.d_score_min();
    profile.d_shift_min = profile_config.d_shift_min();
    profile.kappa_den = profile_config.kappa_den();
    profile.s_prior = profile_config.s_prior;
    profile.phi_over = profile_config.phi_over;

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

bool PredictServiceableModel(const RatioDetectorCoreSpec& spec,
                             const std::string& series_key,
                             const RatioSeriesRuntimeState& state,
                             int64_t bucket_id,
                             FormalPrediction* out_prediction,
                             std::shared_ptr<RatioFormalModel>* out_model,
                             ShadowRefKind* out_ref_kind,
                             bool source_kind) {
    FormalPrediction prediction;
    if (state.formal_state.formal_ready &&
        PredictRatioModel(spec, series_key, state.formal_model.get(), bucket_id, &prediction) ==
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

SelectedRatioBaseline ResolveServiceableBaseline(
    const RatioDetectorCoreSpec& spec,
    const std::string& key,
    int64_t bucket_id,
    const RatioSeriesRuntimeState& self_runtime_state,
    const std::vector<RatioSourceRuntimeView>& source_runtime_states,
    const BaselineSourceConfig* baseline_source_config) {
    SelectedRatioBaseline selected;

    // Sprint 20 BaselineA 对比例特征明确收口为 formal-only 来源：
    // 先尝试 self formal，再按静态配置依次尝试 configured source formal。
    // candidate model 仍可保留给慢路径状态观测，但不参与正式来源决策。
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
                                      [&source_ref](const RatioSourceRuntimeView& view) {
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
    const SharedProfileConfig config = SharedConfig();
    if (abs_residual <= config.z_warn) return 0.0;
    return ClipUnit((abs_residual - config.z_warn) / (config.z_crit - config.z_warn));
}

double ComputeNormalizedScore(double score_point, double score_shift) {
    return 1.0 - (1.0 - score_point) * (1.0 - SharedConfig().w_shift * score_shift);
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

BaselineStringRef StringRefOf(const std::string& value) {
    if (value.empty()) return BaselineStringRef{};
    return BaselineStringRef{value.c_str(), static_cast<uint32_t>(value.size())};
}

void FillResultIdentity(const RatioDetectorCoreSpec& spec,
                        const RatioFeatureProfile& profile,
                        const RatioObservation& obs,
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

double ClipProbability(double value, double eps_logit) {
    return std::min(1.0 - eps_logit, std::max(eps_logit, value));
}

double Logit(double value, double eps_logit) {
    const double clipped = ClipProbability(value, eps_logit);
    return std::log(clipped / (1.0 - clipped));
}

double ComputeSmoothedRatio(const RatioObservation& obs,
                            const RatioFormalModel& model,
                            double eps_logit) {
    return ClipProbability((obs.numerator + model.alpha0) /
                               (obs.denominator + model.alpha0 + model.beta0),
                           eps_logit);
}

double ComputeEffectiveVariance(const RatioFeatureProfile& profile,
                                const RatioProfileConfig& profile_config,
                                double denominator,
                                double p_hat_t) {
    const double var_ideal = denominator * p_hat_t * (1.0 - p_hat_t);
    const double var_model = var_ideal * profile.phi_over;
    return std::max(profile_config.v_floor, var_model);
}

void FillRatioEvidence(DetectorResult* out,
                       const RatioSeriesRuntimeState& runtime_state,
                       const RatioObservation& obs,
                       double p_smooth,
                       double x_t,
                       double p_hat_t,
                       double var_eff_t,
                       double r_t,
                       double rho_t,
                       double score_point,
                       double score_shift,
                       bool shadow_active) {
    if (!out) return;

    out->evidence.kind = BaselineEvidenceKind::kRatio;
    out->evidence.ratio = RatioEvidence{};

    RatioEvidence& evidence = out->evidence.ratio;
    evidence.numerator = obs.numerator;
    evidence.denominator = obs.denominator;
    evidence.p_smooth = p_smooth;
    evidence.x_t = x_t;
    evidence.p_hat_t = p_hat_t;
    evidence.var_eff_t = var_eff_t;
    evidence.r_t = r_t;
    evidence.rho_t = rho_t;
    evidence.p_shift_t = runtime_state.last_p_shift;
    evidence.dir_t = DriftDirectionSign(runtime_state.drift_state.direction);
    evidence.score_point = score_point;
    evidence.score_shift = score_shift;
    evidence.baseline_source_kind = runtime_state.baseline_source.selected_kind;
    evidence.model_state = EvidenceModelState(runtime_state.model_state);
    evidence.shadow_active = shadow_active;

    if (runtime_state.baseline_source.selected_kind == BaselineSourceKind::kConfiguredSource &&
        !runtime_state.baseline_source.selected_source_key.empty()) {
        evidence.field_flags |= kBaselineEvidenceHasSourceKey;
        evidence.baseline_source_key =
            StringRefOf(runtime_state.baseline_source.selected_source_key);
    }
}

}  // namespace

RatioDetectorCore::RatioDetectorCore(const RatioDetectorCoreSpec& spec)
    : spec_(spec) {
    std::string err;
    if (!BuildProfile(spec_, &profile_, &err)) {
        profile_.feature_type = spec_.feature_type;
        profile_.feature_profile = spec_.feature_profile;
    }

    // 本 core 的 feature 固定，来源配置必须按运行时 key 单独生效，
    // 避免一个 task 下未配置的 key 误借用其他 key 的基线来源。
    for (const auto& source_config : spec_.baseline_source_configs) {
        baseline_source_config_by_key_.emplace(source_config.key, source_config.config);
    }
}

int RatioDetectorCore::Submit(const RatioObservation& obs,
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

    const double observed_ratio = ComputeObservedRatio(obs);
    const double rho_t = ComputeRho(profile_, obs.denominator);
    const bool gate_train = obs.denominator >= static_cast<double>(profile_.d_min_train);
    const bool gate_score = obs.denominator >= static_cast<double>(profile_.d_score_min);
    const bool gate_shift = obs.denominator >= static_cast<double>(profile_.d_shift_min);
    const std::string key = CopyKey(obs.key);
    const BaselineSourceConfig* baseline_source_config = nullptr;
    auto source_config_it = baseline_source_config_by_key_.find(key);
    if (source_config_it != baseline_source_config_by_key_.end()) {
        baseline_source_config = &source_config_it->second;
    }

    bool enqueue_rebuild = false;
    int64_t rebuild_start_hint = 0;
    const std::size_t shadow_fit_points = ShadowMinPointsForCandidate();
    const std::size_t shadow_holdout_points = ShadowMinHoldoutPoints();
    const std::size_t shadow_window_points = shadow_fit_points + shadow_holdout_points;
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
        UpdateCoverageStats(&runtime_state.readiness_state, obs.bucket_id, gate_train);
        double p_smooth = 0.0;
        double x_t = 0.0;
        double p_hat_t = 0.0;
        double var_eff_t = 0.0;
        double residual = 0.0;
        double score_point = 0.0;
        double score_shift = 0.0;
        DriftDirection drift_direction = runtime_state.drift_state.direction;
        bool serviceable = false;
        bool shadow_active = false;
        RatioProfileConfig profile_config;
        (void)GetRatioProfileConfig(profile_.feature_profile, &profile_config);
        const SharedProfileConfig shared_config = SharedConfig();
        const DriftConfig& drift_config = shared_config.drift;

        if (update.gap > drift_config.g_reset) {
            if (runtime_state.shadow_state.active) {
                runtime_state.shadow_state.Reset();
                runtime_state.formal_state.full_waiting = false;
                runtime_state.formal_state.candidate_passed = false;
                runtime_state.formal_state.t_switch_gate = -1;
                runtime_state.formal_state.t_full_start = -1;
            } else {
                ResetSlowDriftTracking(&runtime_state.shadow_state);
            }
        }

        // shadow 分支直接沿用冻结参考模型的概率预测，再叠加在线 delta。
        // 对比例特征来说，delta 作用在概率空间，最终仍然按低分母放大的方差层做标准化。
        if (runtime_state.shadow_state.active &&
            runtime_state.shadow_state.frozen_ref_model) {
            FormalPrediction shadow_prediction;
            const std::string shadow_key =
                ShadowRefUsesSource(runtime_state.shadow_state.ref_kind) &&
                        !runtime_state.shadow_state.ref_source_key.empty()
                    ? runtime_state.shadow_state.ref_source_key
                    : key;
            if (PredictRatioModel(spec_,
                                  shadow_key,
                                  runtime_state.shadow_state.frozen_ref_model.get(),
                                  obs.bucket_id,
                                  &shadow_prediction) == error::OK &&
                shadow_prediction.ready) {
                serviceable = true;
                shadow_active = true;
                const RatioFormalModel& shadow_model =
                    *runtime_state.shadow_state.frozen_ref_model;
                p_smooth = ComputeSmoothedRatio(obs, shadow_model, profile_config.eps_logit);
                x_t = Logit(p_smooth, profile_config.eps_logit);
                p_hat_t = ClipProbability(
                    shadow_prediction.value + runtime_state.shadow_state.delta,
                    profile_config.eps_logit);
                var_eff_t = ComputeEffectiveVariance(
                    profile_, profile_config, obs.denominator, p_hat_t);
                residual =
                    (obs.numerator - obs.denominator * p_hat_t) / std::sqrt(var_eff_t);

                runtime_state.model_state =
                    ShadowRefUsesSource(runtime_state.shadow_state.ref_kind)
                        ? "shadow_source"
                        : "shadow_self";
                runtime_state.baseline_source.selected_kind =
                    SourceKindFromShadowRef(runtime_state.shadow_state.ref_kind);
                runtime_state.baseline_source.selected_source_key =
                    runtime_state.shadow_state.ref_source_key;
                runtime_state.baseline_source.serviceable = true;
                out_submit->detector_result.provider = BaselineProvider::kShadow;
                out_submit->detector_result.flags |= kBaselineFlagShadowActive;
                RefreshOnlineReadiness(&runtime_state.readiness_state,
                                       shared_config,
                                       shadow_prediction.readiness,
                                       runtime_state.baseline_source.selected_kind);
                UpdateSlowDriftTracking(
                    &runtime_state.shadow_state, shadow_holdout_points, residual, 1.0);

                if (gate_score) {
                    out_submit->detector_result.raw_score =
                        std::fabs(residual) / RatioShadowScoreScale();
                    score_point = ComputePointScoreFromAbsResidual(
                        out_submit->detector_result.raw_score);
                }
                runtime_state.shadow_state.delta =
                    (1.0 - drift_config.alpha) * runtime_state.shadow_state.delta +
                    drift_config.alpha * (observed_ratio - shadow_prediction.value);
                runtime_state.shadow_state.last_bucket_id = obs.bucket_id;
            } else {
                runtime_state.shadow_state.Reset();
            }
        }

        if (!serviceable) {
            // 常态路径先决出“当前 bucket 用谁来解释”，然后才进入残差 / 漂移评分。
            // 如果 self 与 source 都不可服务，热路径只推进公共时序状态，不制造伪异常。
            std::vector<RatioSourceRuntimeView> source_runtime_states;
            if (baseline_source_config) {
                source_runtime_states.reserve(baseline_source_config->sources.size());
                for (const auto& source_ref : baseline_source_config->sources) {
                    const RuntimeShardState& source_shard =
                        runtime_shards_[RuntimeShardIndex(source_ref.source_key)];
                    auto source_it = source_shard.states.find(source_ref.source_key);
                    if (source_it == source_shard.states.end()) continue;
                    source_runtime_states.push_back(
                        RatioSourceRuntimeView{source_ref.source_key, source_it->second.runtime_state});
                }
            }
            const SelectedRatioBaseline selected = ResolveServiceableBaseline(
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
                p_smooth =
                    ComputeSmoothedRatio(obs, *selected.model, profile_config.eps_logit);
                x_t = Logit(p_smooth, profile_config.eps_logit);
                p_hat_t = ClipProbability(selected.prediction.value, profile_config.eps_logit);
                var_eff_t = ComputeEffectiveVariance(
                    profile_, profile_config, obs.denominator, p_hat_t);
                residual =
                    (obs.numerator - obs.denominator * p_hat_t) / std::sqrt(var_eff_t);
                RefreshOnlineReadiness(&runtime_state.readiness_state,
                                       shared_config,
                                       selected.prediction.readiness,
                                       selected.decision.selected_kind);
                UpdateSlowDriftTracking(
                    &runtime_state.shadow_state, shadow_holdout_points, residual, 1.0);

                if (gate_score) {
                    out_submit->detector_result.raw_score = std::fabs(residual);
                    score_point = ComputePointScoreFromAbsResidual(
                        out_submit->detector_result.raw_score);
                }

                const DriftUpdateResult drift_result = UpdateDriftState(
                    &runtime_state.drift_state,
                    drift_config,
                    obs.bucket_id,
                    residual,
                    gate_shift);
                runtime_state.last_p_shift = drift_result.p_shift;
                runtime_state.last_shift_confirmed = drift_result.shift_confirmed;
                drift_direction = drift_result.direction;
                score_shift =
                    ClipUnit((drift_result.p_shift - drift_config.p_shift_low) /
                             (drift_config.p_shift_high - drift_config.p_shift_low));

                const bool gate_fast =
                    gate_shift && std::fabs(residual) >= ShadowZShiftConfirmMin() &&
                    runtime_state.drift_state.confirm_count >= ShadowCRebuildMin();
                const bool gate_slow = runtime_state.shadow_state.slow_drift_triggered;
                const bool existing_drift_evidence =
                    drift_result.shift_confirmed &&
                    runtime_state.drift_state.confirm_count >= drift_config.m_shift;

                // formal -> shadow 仅做保护，不立即触发 candidate 重建。
                if (existing_drift_evidence && (gate_fast || gate_slow) &&
                    !runtime_state.shadow_state.active &&
                    !runtime_state.shift_rebuild_pending &&
                    selected.model) {
                    runtime_state.shadow_state.Reset();
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
                    runtime_state.shadow_state.delta = observed_ratio - p_hat_t;
                    runtime_state.shadow_state.last_bucket_id = obs.bucket_id;
                    runtime_state.shadow_state.enter_bucket_id = obs.bucket_id;
                    runtime_state.model_state =
                        ShadowRefUsesSource(selected.shadow_ref_kind)
                            ? "shadow_source"
                            : "shadow_self";
                    runtime_state.formal_state.switch_state =
                        RebuildSwitchState::kShadowActive;
                    runtime_state.formal_state.failure_reason =
                        RebuildFailureReason::kNone;
                    runtime_state.formal_state.failure_reason_detail.clear();
                    runtime_state.formal_state.candidate_passed = false;
                    runtime_state.formal_state.full_waiting = false;
                    runtime_state.formal_state.t_switch_gate = -1;
                    runtime_state.formal_state.t_full_start = -1;

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
                confidence_base = std::min(confidence_base, RatioShadowConfidenceCap());
            }
            out_submit->detector_result.confidence =
                confidence_base / std::max(rho_t, 1.0);

            FillRatioEvidence(&out_submit->detector_result,
                              runtime_state,
                              obs,
                              p_smooth,
                              x_t,
                              p_hat_t,
                              var_eff_t,
                              residual,
                              rho_t,
                              score_point,
                              score_shift,
                              shadow_active);
        }

        if (serviceable && runtime_state.shadow_state.active) {
            if (runtime_state.shadow_state.enter_bucket_id < 0) {
                runtime_state.shadow_state.enter_bucket_id = obs.bucket_id;
            }
            ++runtime_state.shadow_state.shadow_point_count;
            runtime_state.shadow_state.shadow_effective_holdout_count =
                runtime_state.shadow_state.shadow_point_count > shadow_fit_points
                    ? runtime_state.shadow_state.shadow_point_count - shadow_fit_points
                    : 0;
            runtime_state.shadow_state.shadow_stuck =
                runtime_state.shadow_state.shadow_point_count > ShadowStuckAlertPoints();

            const bool gate_data_fit =
                runtime_state.shadow_state.shadow_point_count >= shadow_fit_points;
            const bool gate_data_val =
                runtime_state.shadow_state.shadow_effective_holdout_count >=
                shadow_holdout_points;
            const bool gate_data = gate_data_fit && gate_data_val;
            const bool gate_fast =
                gate_shift && std::fabs(residual) >= ShadowZShiftConfirmMin() &&
                runtime_state.drift_state.confirm_count >= ShadowCRebuildMin();
            const bool gate_drift = gate_fast || runtime_state.shadow_state.slow_drift_triggered;
            const int64_t last_attempt = runtime_state.shadow_state.last_candidate_attempt_bucket;
            const bool gate_cooldown =
                last_attempt < 0 ||
                obs.bucket_id - last_attempt >=
                    static_cast<int64_t>(ShadowRetryCooldownPoints());
            const bool gate_no_pending = !runtime_state.shift_rebuild_pending;
            const bool gate_not_waiting = !runtime_state.formal_state.full_waiting;
            if (!enqueue_rebuild && gate_no_pending && gate_not_waiting &&
                gate_data && gate_drift && gate_cooldown) {
                enqueue_rebuild = true;
                rebuild_start_hint = std::max<int64_t>(
                    0,
                    obs.bucket_id - static_cast<int64_t>(shadow_window_points) + 1);
            }
        }

        NormalizeSwitchState(
            &runtime_state.formal_state, runtime_state.shadow_state.active);

        runtime_state.last_numerator = obs.numerator;
        runtime_state.last_denominator = obs.denominator;
        runtime_state.last_observed_ratio = observed_ratio;
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
                                 [bucket_id = obs.bucket_id](const RatioSeriesShardEntry& entry) {
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

int RatioDetectorCore::BuildSeriesSnapshot(const BaselineStringRef& key,
                                           RatioSeriesSnapshot* out_snapshot) const {
    if (!out_snapshot) return error::BAD_REQUEST;

    const std::string key_copy = CopyKey(key);
    if (key_copy.empty()) return error::BAD_REQUEST;

    RatioSeriesShardEntry entry;
    const RuntimeShardState& shard = runtime_shards_[RuntimeShardIndex(key_copy)];
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto it = shard.states.find(key_copy);
        if (it == shard.states.end() || !it->second.series_state.initialized) {
            return error::NOT_FOUND;
        }
        entry = it->second;
    }

    RatioSeriesSnapshot snapshot;
    snapshot.series_state = entry.series_state;
    snapshot.runtime_state = entry.runtime_state;
    const RatioSeriesRuntimeState& runtime_state = snapshot.runtime_state;
    const std::string& series_key = key_copy;
    snapshot.formal_predict_status = PredictRatioModel(
        spec_,
        series_key,
        runtime_state.formal_model.get(),
        snapshot.series_state.last_bucket_id,
        &snapshot.formal_prediction);
    snapshot.candidate_predict_status = PredictRatioModel(
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

int RatioDetectorCore::GetSeriesState(const BaselineStringRef& key,
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

int RatioDetectorCore::BuildRebuildContext(const std::string& key,
                                           RatioRebuildContext* out_context) const {
    if (!out_context) return error::BAD_REQUEST;

    RatioRebuildContext context;
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

void RatioDetectorCore::MarkRebuildEnqueued(const std::string& key) {
    RuntimeShardState& shard = runtime_shards_[RuntimeShardIndex(key)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto& runtime_state = shard.states[key].runtime_state;
    runtime_state.shift_rebuild_pending = true;
    BeginRebuildCycle(&runtime_state.formal_state, RebuildSwitchState::kRebuildPending);
    runtime_state.formal_state.candidate_state = RebuildCandidateState::kBuilding;
    MarkRebuildStageBuilding(&runtime_state.formal_state.stage_trace);
}

void RatioDetectorCore::MarkCandidateAttemptEnqueued(const std::string& key, int64_t bucket_id) {
    RuntimeShardState& shard = runtime_shards_[RuntimeShardIndex(key)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.states.find(key);
    if (it == shard.states.end()) return;
    if (!it->second.runtime_state.shadow_state.active) return;
    it->second.runtime_state.shadow_state.last_candidate_attempt_bucket = bucket_id;
}

void RatioDetectorCore::MarkCandidateBuilding(const std::string& key) {
    RuntimeShardState& shard = runtime_shards_[RuntimeShardIndex(key)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto& runtime_state = shard.states[key].runtime_state;
    runtime_state.shift_rebuild_pending = true;
    runtime_state.formal_state.candidate_state = RebuildCandidateState::kBuilding;
    runtime_state.formal_state.switch_state = RebuildSwitchState::kRebuildPending;
    runtime_state.formal_state.failure_reason = RebuildFailureReason::kNone;
    runtime_state.formal_state.failure_reason_detail.clear();
    runtime_state.formal_state.candidate_passed = false;
    runtime_state.formal_state.full_waiting = false;
    runtime_state.formal_state.t_switch_gate = -1;
    runtime_state.formal_state.t_full_start = -1;
    ResetCandidateSnapshot(&runtime_state.formal_state);
    MarkRebuildStageBuilding(&runtime_state.formal_state.stage_trace);
}

void RatioDetectorCore::MarkCandidateBuilt(const std::string& key,
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

void RatioDetectorCore::MarkCandidateValidating(const std::string& key,
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

void RatioDetectorCore::ApplyFormalModel(
    const std::string& key,
    const RatioApplyFormalModelResult& apply_result) {
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
        if (apply_result.full_waiting) {
            runtime_state.candidate_replay.reset();
            runtime_state.candidate_model.reset();
        }
        // `replace_formal_model` 的语义与数值特征一致：新 formal 一旦切换成功，
        // 就把旧 shadow / drift / candidate 状态整体丢弃，避免旧概率语义继续生效。
        if (!apply_result.full_waiting && apply_result.replace_formal_model) {
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
            runtime_state.formal_state.candidate_passed = false;
            runtime_state.formal_state.full_waiting = false;
            runtime_state.formal_state.t_switch_gate = -1;
            runtime_state.formal_state.t_full_start = -1;
        } else if (!apply_result.full_waiting && apply_result.full_model) {
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
            runtime_state.formal_state.candidate_passed = false;
            runtime_state.formal_state.full_waiting = false;
            runtime_state.formal_state.t_switch_gate = -1;
            runtime_state.formal_state.t_full_start = -1;
        } else if (!apply_result.full_waiting) {
            runtime_state.candidate_replay.reset();
            runtime_state.candidate_model.reset();
        }
    } else {
        runtime_state.candidate_replay.reset();
        runtime_state.candidate_model.reset();
    }

    NormalizeSwitchState(
        &runtime_state.formal_state, runtime_state.shadow_state.active);
}

void RatioDetectorCore::MarkRebuildFailure(const DetectorRebuildFailure& failure) {
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
    NormalizeSwitchState(
        &runtime_state.formal_state, runtime_state.shadow_state.active);
}

void RatioDetectorCore::ClearPendingRebuild(const std::string& key) {
    RuntimeShardState& shard = runtime_shards_[RuntimeShardIndex(key)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.states.find(key);
    if (it == shard.states.end()) return;
    it->second.runtime_state.shift_rebuild_pending = false;
    FormalModelState& formal_state = it->second.runtime_state.formal_state;
    formal_state.candidate_state = RebuildCandidateState::kNone;
    formal_state.failure_reason = RebuildFailureReason::kNone;
    formal_state.failure_reason_detail.clear();
    formal_state.full_waiting = false;
    formal_state.candidate_passed = false;
    formal_state.t_switch_gate = -1;
    formal_state.t_full_start = -1;
    NormalizeSwitchState(
        &formal_state, it->second.runtime_state.shadow_state.active);
    ResetRebuildStageTrace(&formal_state.stage_trace);
    ResetCandidateSnapshot(&formal_state);
}

void RatioDetectorCore::Clear() {
    for (auto& shard : runtime_shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.states.clear();
        shard.prune_cursor = 0;
    }
    prune_cursor_.store(0, std::memory_order_relaxed);
    last_pruned_bucket_.store(-1, std::memory_order_relaxed);
    pruned_key_count_total_.store(0, std::memory_order_relaxed);
}

size_t RatioDetectorCore::Size() const {
    size_t total = 0;
    for (const auto& shard : runtime_shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (const auto& entry : shard.states) {
            if (entry.second.series_state.initialized) ++total;
        }
    }
    return total;
}

uint64_t RatioDetectorCore::PrunedKeyCount() const {
    return pruned_key_count_total_.load(std::memory_order_relaxed);
}

int64_t RatioDetectorCore::IdlePruneBucketGap() const {
    return RuntimeIdlePruneBucketGap();
}

}  // namespace baseline
}  // namespace flowsql
