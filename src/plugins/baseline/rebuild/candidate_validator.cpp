/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "candidate_validator.h"

#include <algorithm>
#include <common/error_code.h>

#include <cmath>

#include "plugins/baseline/config/runtime_config.h"
#include "plugins/baseline/model/formal_predictor.h"
#include "plugins/baseline/model/profile_config.h"

namespace flowsql {
namespace baseline {

namespace {

double HuberLoss(double residual) {
    const double huber_delta = CandidateHuberDelta();
    const double abs_residual = std::fabs(residual);
    if (abs_residual <= huber_delta) {
        return 0.5 * abs_residual * abs_residual;
    }
    return huber_delta * (abs_residual - 0.5 * huber_delta);
}

double RatioClipEps() {
    double eps = kRatioEpsLogit;
    (void)TryGetRatioGlobalNumericalOverride(&eps, nullptr, nullptr);
    return eps;
}

std::size_t ValidationStartIndex(std::size_t total_count,
                                 const ReplayWindowSummary& holdout_window) {
    const std::size_t holdout_count = static_cast<std::size_t>(holdout_window.observation_count);
    if (holdout_count == 0 || holdout_count > total_count) return total_count;
    return total_count - holdout_count;
}

bool PredictValueReady(const ValueFormalModel* model,
                       const BaselineTaskSpec* task_spec,
                       const CompiledEventCalendar* compiled_event_calendar,
                       int64_t bucket_id,
                       double* out_value) {
    FormalPredictContext context;
    context.task_spec = task_spec;
    context.event_calendar = compiled_event_calendar;
    context.bucket_id = bucket_id;
    FormalPrediction prediction;
    const int rc = PredictFormalModel(model, context, &prediction);
    if (rc != error::OK || !prediction.ready) return false;
    if (out_value) *out_value = prediction.value;
    return true;
}

bool PredictRatioReady(const RatioFormalModel* model,
                       const BaselineTaskSpec* task_spec,
                       const CompiledEventCalendar* compiled_event_calendar,
                       int64_t bucket_id,
                       double* out_value) {
    FormalPredictContext context;
    context.task_spec = task_spec;
    context.event_calendar = compiled_event_calendar;
    context.bucket_id = bucket_id;
    FormalPrediction prediction;
    const int rc = PredictFormalModel(model, context, &prediction);
    if (rc != error::OK || !prediction.ready) return false;
    if (out_value) *out_value = prediction.value;
    return true;
}

CandidateValidationResult FinalizeValidationResult(CandidateValidationResult result) {
    if (result.validation_count == 0) {
        result.status = CandidateValidationStatus::kInsufficientHoldout;
        result.pass = false;
        return result;
    }

    const SharedProfileConfig shared_config = DefaultSharedProfileConfig();
    // 切换判定不要求 candidate 严格优于 incumbent。
    // 当两边 holdout 损失都已经接近 0 时，纯相对比较会被浮点噪声放大，
    // 使“数值上等价”的候选模型被误拒绝。这里补一个极小的绝对容忍带，
    // 仅用于 near-zero 区间稳定 formal switch，不改变正常量级下的相对判定语义。
    const double tolerance =
        std::max(CandidateSwitchLossAbsTol(), shared_config.eps_switch * result.incumbent_loss);
    result.pass = result.candidate_loss <= (result.incumbent_loss + tolerance);
    result.status = result.pass ? CandidateValidationStatus::kPassed
                                : CandidateValidationStatus::kFailed;
    return result;
}

}  // namespace

const char* CandidateValidationStatusName(CandidateValidationStatus status) {
    switch (status) {
        case CandidateValidationStatus::kBypassNoIncumbent:
            return "bypass_no_incumbent";
        case CandidateValidationStatus::kPassed:
            return "passed";
        case CandidateValidationStatus::kFailed:
            return "failed";
        case CandidateValidationStatus::kInsufficientHoldout:
            return "insufficient_holdout";
        case CandidateValidationStatus::kUnavailableIncumbent:
            return "unavailable_incumbent";
        case CandidateValidationStatus::kNone:
            break;
    }
    return "none";
}

CandidateValidationResult CandidateValidator::ValidateValue(
    const ValueFeatureProfile& profile,
    const ValueReplaySeries& replay,
    const ReplayWindowSummary& holdout_window,
    const ValueFormalModel* candidate_model,
    const ValueFormalModel* incumbent_formal_model,
    const ValueShadowState* incumbent_shadow_state,
    const BaselineTaskSpec* task_spec,
    const CompiledEventCalendar* compiled_event_calendar) {
    if (!candidate_model) {
        return CandidateValidationResult{CandidateValidationStatus::kFailed, false};
    }
    if (!incumbent_formal_model && (!incumbent_shadow_state || !incumbent_shadow_state->active)) {
        return CandidateValidationResult{CandidateValidationStatus::kBypassNoIncumbent, true};
    }

    const std::size_t val_begin = ValidationStartIndex(replay.points.size(), holdout_window);
    if (val_begin >= replay.points.size()) {
        return CandidateValidationResult{CandidateValidationStatus::kInsufficientHoldout, false};
    }

    CandidateValidationResult result;

    if (incumbent_shadow_state && incumbent_shadow_state->active) {
        if (!incumbent_shadow_state->frozen_ref_model) {
            return CandidateValidationResult{CandidateValidationStatus::kUnavailableIncumbent, false};
        }

        // incumbent 若正处于 shadow 接管期，就不能拿旧 formal 直接计损。
        // 这里按在线同一规则做一次单遍 replay：先用冻结 ref + delta 预测，
        // 在 holdout 段上“先计损、后更新 delta”，保持与线上 prequential 语义一致。
        double delta = 0.0;
        bool delta_initialized = false;
        for (std::size_t i = 0; i < replay.points.size(); ++i) {
            const auto& point = replay.points[i];
            if (profile.is_sampled && point.sample_count < profile.n_score_min) continue;

            double mu_ref = 0.0;
            if (!PredictValueReady(incumbent_shadow_state->frozen_ref_model.get(),
                                   task_spec,
                                   compiled_event_calendar,
                                   point.bucket_id,
                                   &mu_ref)) {
                return CandidateValidationResult{CandidateValidationStatus::kUnavailableIncumbent, false};
            }

            const double x_t = TransformValueObservation(profile, point.value);
            if (!delta_initialized) {
                delta = x_t - mu_ref;
                delta_initialized = true;
            }
            const double mu_shadow = mu_ref + delta;

            if (i >= val_begin) {
                double mu_candidate = 0.0;
                if (!PredictValueReady(candidate_model,
                                       task_spec,
                                       compiled_event_calendar,
                                       point.bucket_id,
                                       &mu_candidate)) {
                    return CandidateValidationResult{CandidateValidationStatus::kFailed, false};
                }

                result.candidate_loss += HuberLoss(x_t - mu_candidate);
                result.incumbent_loss += HuberLoss(x_t - mu_shadow);
                ++result.validation_count;
            }

            const double shadow_alpha = CandidateShadowAlpha();
            delta = (1.0 - shadow_alpha) * delta + shadow_alpha * (x_t - mu_ref);
        }
        return FinalizeValidationResult(result);
    }

    if (!incumbent_formal_model) {
        return CandidateValidationResult{CandidateValidationStatus::kUnavailableIncumbent, false};
    }

    for (std::size_t i = val_begin; i < replay.points.size(); ++i) {
        const auto& point = replay.points[i];
        if (profile.is_sampled && point.sample_count < profile.n_score_min) continue;

        double mu_candidate = 0.0;
        double mu_incumbent = 0.0;
        if (!PredictValueReady(candidate_model,
                               task_spec,
                               compiled_event_calendar,
                               point.bucket_id,
                               &mu_candidate) ||
            !PredictValueReady(incumbent_formal_model,
                               task_spec,
                               compiled_event_calendar,
                               point.bucket_id,
                               &mu_incumbent)) {
            return CandidateValidationResult{CandidateValidationStatus::kUnavailableIncumbent, false};
        }

        const double x_t = TransformValueObservation(profile, point.value);
        result.candidate_loss += HuberLoss(x_t - mu_candidate);
        result.incumbent_loss += HuberLoss(x_t - mu_incumbent);
        ++result.validation_count;
    }
    return FinalizeValidationResult(result);
}

CandidateValidationResult CandidateValidator::ValidateRatio(
    const RatioFeatureProfile& profile,
    const RatioReplaySeries& replay,
    const ReplayWindowSummary& holdout_window,
    const RatioFormalModel* candidate_model,
    const RatioFormalModel* incumbent_formal_model,
    const RatioShadowState* incumbent_shadow_state,
    const BaselineTaskSpec* task_spec,
    const CompiledEventCalendar* compiled_event_calendar) {
    if (!candidate_model) {
        return CandidateValidationResult{CandidateValidationStatus::kFailed, false};
    }
    if (!incumbent_formal_model && (!incumbent_shadow_state || !incumbent_shadow_state->active)) {
        return CandidateValidationResult{CandidateValidationStatus::kBypassNoIncumbent, true};
    }

    const std::size_t val_begin = ValidationStartIndex(replay.points.size(), holdout_window);
    if (val_begin >= replay.points.size()) {
        return CandidateValidationResult{CandidateValidationStatus::kInsufficientHoldout, false};
    }

    CandidateValidationResult result;

    if (incumbent_shadow_state && incumbent_shadow_state->active) {
        if (!incumbent_shadow_state->frozen_ref_model) {
            return CandidateValidationResult{CandidateValidationStatus::kUnavailableIncumbent, false};
        }

        // 比例特征的 shadow replay 与数值特征相同，也是单遍 prequential 复现。
        // 不同点仅在于 delta 作用于比例空间，计损时继续保留低分母方差层。
        double delta = 0.0;
        bool delta_initialized = false;
        for (std::size_t i = 0; i < replay.points.size(); ++i) {
            const auto& point = replay.points[i];
            if (point.denominator < static_cast<double>(profile.d_score_min)) continue;

            double mu_ref = 0.0;
            if (!PredictRatioReady(incumbent_shadow_state->frozen_ref_model.get(),
                                   task_spec,
                                   compiled_event_calendar,
                                   point.bucket_id,
                                   &mu_ref)) {
                return CandidateValidationResult{CandidateValidationStatus::kUnavailableIncumbent, false};
            }

            const double observed_ratio = point.numerator / point.denominator;
            if (!delta_initialized) {
                delta = observed_ratio - mu_ref;
                delta_initialized = true;
            }
            const double ratio_eps = RatioClipEps();
            const double mu_shadow =
                std::min(1.0 - ratio_eps, std::max(ratio_eps, mu_ref + delta));

            if (i >= val_begin) {
                double mu_candidate = 0.0;
                if (!PredictRatioReady(candidate_model,
                                       task_spec,
                                       compiled_event_calendar,
                                       point.bucket_id,
                                       &mu_candidate)) {
                    return CandidateValidationResult{CandidateValidationStatus::kFailed, false};
                }

                const double candidate_p =
                    std::min(1.0 - ratio_eps, std::max(ratio_eps, mu_candidate));
                const double weight =
                    point.denominator /
                    (point.denominator + static_cast<double>(profile.d_min_train));
                const double var_candidate = std::max(
                    CandidateRatioVarianceFloor(),
                    profile.phi_over * point.denominator * candidate_p * (1.0 - candidate_p));
                const double var_incumbent = std::max(
                    CandidateRatioVarianceFloor(),
                    profile.phi_over * point.denominator * mu_shadow * (1.0 - mu_shadow));

                result.candidate_loss += weight * HuberLoss(
                    (point.numerator - point.denominator * candidate_p) / std::sqrt(var_candidate));
                result.incumbent_loss += weight * HuberLoss(
                    (point.numerator - point.denominator * mu_shadow) / std::sqrt(var_incumbent));
                ++result.validation_count;
            }

            const double shadow_alpha = CandidateShadowAlpha();
            delta = (1.0 - shadow_alpha) * delta + shadow_alpha * (observed_ratio - mu_ref);
        }
        return FinalizeValidationResult(result);
    }

    if (!incumbent_formal_model) {
        return CandidateValidationResult{CandidateValidationStatus::kUnavailableIncumbent, false};
    }

    for (std::size_t i = val_begin; i < replay.points.size(); ++i) {
        const auto& point = replay.points[i];
        if (point.denominator < static_cast<double>(profile.d_score_min)) continue;

        double mu_candidate = 0.0;
        double mu_incumbent = 0.0;
        if (!PredictRatioReady(candidate_model,
                               task_spec,
                               compiled_event_calendar,
                               point.bucket_id,
                               &mu_candidate) ||
            !PredictRatioReady(incumbent_formal_model,
                               task_spec,
                               compiled_event_calendar,
                               point.bucket_id,
                               &mu_incumbent)) {
            return CandidateValidationResult{CandidateValidationStatus::kUnavailableIncumbent, false};
        }

        const double ratio_eps = RatioClipEps();
        const double candidate_p = std::min(1.0 - ratio_eps, std::max(ratio_eps, mu_candidate));
        const double incumbent_p = std::min(1.0 - ratio_eps, std::max(ratio_eps, mu_incumbent));
        const double weight =
            point.denominator /
            (point.denominator + static_cast<double>(profile.d_min_train));
        const double var_candidate = std::max(
            CandidateRatioVarianceFloor(),
            profile.phi_over * point.denominator * candidate_p * (1.0 - candidate_p));
        const double var_incumbent = std::max(
            CandidateRatioVarianceFloor(),
            profile.phi_over * point.denominator * incumbent_p * (1.0 - incumbent_p));

        result.candidate_loss += weight * HuberLoss(
            (point.numerator - point.denominator * candidate_p) / std::sqrt(var_candidate));
        result.incumbent_loss += weight * HuberLoss(
            (point.numerator - point.denominator * incumbent_p) / std::sqrt(var_incumbent));
        ++result.validation_count;
    }
    return FinalizeValidationResult(result);
}

CandidateValidationResult CandidateValidator::ValidateRelationAggregate(
    double candidate_loss_sum,
    double incumbent_loss_sum,
    uint64_t validation_feature_count) {
    CandidateValidationResult result;
    result.validation_count = validation_feature_count;

    if (validation_feature_count > 0) {
        const double denom = static_cast<double>(validation_feature_count);
        result.candidate_loss = candidate_loss_sum / denom;
        result.incumbent_loss = incumbent_loss_sum / denom;
    }

    return FinalizeValidationResult(result);
}

}  // namespace baseline
}  // namespace flowsql
