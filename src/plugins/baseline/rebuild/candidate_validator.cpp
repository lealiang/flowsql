/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "candidate_validator.h"

#include <common/error_code.h>

#include <cmath>

#include "plugins/baseline/model/formal_predictor.h"

namespace flowsql {
namespace baseline {

namespace {

constexpr double kHuberDelta = 1.5;
constexpr double kShadowAlpha = 0.2;
constexpr double kRatioVarianceFloor = 0.25;
constexpr double kSwitchEps = 0.05;

double HuberLoss(double residual) {
    const double abs_residual = std::fabs(residual);
    if (abs_residual <= kHuberDelta) {
        return 0.5 * abs_residual * abs_residual;
    }
    return kHuberDelta * (abs_residual - 0.5 * kHuberDelta);
}

std::size_t ValidationStartIndex(std::size_t total_count,
                                 const ReplayWindowSummary& holdout_window) {
    const std::size_t holdout_count = static_cast<std::size_t>(holdout_window.observation_count);
    if (holdout_count == 0 || holdout_count > total_count) return total_count;
    return total_count - holdout_count;
}

bool PredictValueReady(const ValueFormalModel* model,
                       int64_t bucket_id,
                       double* out_value) {
    FormalPrediction prediction;
    const int rc = PredictFormalModel(model, nullptr, bucket_id, &prediction);
    if (rc != error::OK || !prediction.ready) return false;
    if (out_value) *out_value = prediction.value;
    return true;
}

bool PredictRatioReady(const RatioFormalModel* model,
                       int64_t bucket_id,
                       double* out_value) {
    FormalPrediction prediction;
    const int rc = PredictFormalModel(model, nullptr, bucket_id, &prediction);
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

    // 切换判定不要求 candidate 严格优于 incumbent。
    // 这里保留一个很小的工程容忍带，避免因为有限 holdout、shadow 近似 replay
    // 或数值抖动导致频繁拒绝本来已经足够好的新模型。
    result.pass = result.candidate_loss <= (1.0 + kSwitchEps) * result.incumbent_loss;
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
    const ValueShadowState* incumbent_shadow_state) {
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
            if (profile.is_t1b && point.sample_count < profile.n_score_min) continue;

            double mu_ref = 0.0;
            if (!PredictValueReady(incumbent_shadow_state->frozen_ref_model.get(), point.bucket_id, &mu_ref)) {
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
                if (!PredictValueReady(candidate_model, point.bucket_id, &mu_candidate)) {
                    return CandidateValidationResult{CandidateValidationStatus::kFailed, false};
                }

                result.candidate_loss += HuberLoss(x_t - mu_candidate);
                result.incumbent_loss += HuberLoss(x_t - mu_shadow);
                ++result.validation_count;
            }

            delta = (1.0 - kShadowAlpha) * delta + kShadowAlpha * (x_t - mu_ref);
        }
        return FinalizeValidationResult(result);
    }

    if (!incumbent_formal_model) {
        return CandidateValidationResult{CandidateValidationStatus::kUnavailableIncumbent, false};
    }

    for (std::size_t i = val_begin; i < replay.points.size(); ++i) {
        const auto& point = replay.points[i];
        if (profile.is_t1b && point.sample_count < profile.n_score_min) continue;

        double mu_candidate = 0.0;
        double mu_incumbent = 0.0;
        if (!PredictValueReady(candidate_model, point.bucket_id, &mu_candidate) ||
            !PredictValueReady(incumbent_formal_model, point.bucket_id, &mu_incumbent)) {
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
    const RatioShadowState* incumbent_shadow_state) {
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

        // T2 的 shadow replay 与 T1 相同，也是单遍 prequential 复现。
        // 不同点仅在于 delta 作用于比例空间，计损时继续保留低分母方差层。
        double delta = 0.0;
        bool delta_initialized = false;
        for (std::size_t i = 0; i < replay.points.size(); ++i) {
            const auto& point = replay.points[i];
            if (point.denominator < static_cast<double>(profile.d_score_min)) continue;

            double mu_ref = 0.0;
            if (!PredictRatioReady(incumbent_shadow_state->frozen_ref_model.get(), point.bucket_id, &mu_ref)) {
                return CandidateValidationResult{CandidateValidationStatus::kUnavailableIncumbent, false};
            }

            const double observed_ratio = point.numerator / point.denominator;
            if (!delta_initialized) {
                delta = observed_ratio - mu_ref;
                delta_initialized = true;
            }
            const double mu_shadow = std::min(1.0 - 1e-6, std::max(1e-6, mu_ref + delta));

            if (i >= val_begin) {
                double mu_candidate = 0.0;
                if (!PredictRatioReady(candidate_model, point.bucket_id, &mu_candidate)) {
                    return CandidateValidationResult{CandidateValidationStatus::kFailed, false};
                }

                const double candidate_p = std::min(1.0 - 1e-6, std::max(1e-6, mu_candidate));
                const double weight =
                    point.denominator /
                    (point.denominator + static_cast<double>(profile.d_min_train));
                const double var_candidate = std::max(
                    kRatioVarianceFloor,
                    profile.phi_over * point.denominator * candidate_p * (1.0 - candidate_p));
                const double var_incumbent = std::max(
                    kRatioVarianceFloor,
                    profile.phi_over * point.denominator * mu_shadow * (1.0 - mu_shadow));

                result.candidate_loss += weight * HuberLoss(
                    (point.numerator - point.denominator * candidate_p) / std::sqrt(var_candidate));
                result.incumbent_loss += weight * HuberLoss(
                    (point.numerator - point.denominator * mu_shadow) / std::sqrt(var_incumbent));
                ++result.validation_count;
            }

            delta = (1.0 - kShadowAlpha) * delta + kShadowAlpha * (observed_ratio - mu_ref);
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
        if (!PredictRatioReady(candidate_model, point.bucket_id, &mu_candidate) ||
            !PredictRatioReady(incumbent_formal_model, point.bucket_id, &mu_incumbent)) {
            return CandidateValidationResult{CandidateValidationStatus::kUnavailableIncumbent, false};
        }

        const double candidate_p = std::min(1.0 - 1e-6, std::max(1e-6, mu_candidate));
        const double incumbent_p = std::min(1.0 - 1e-6, std::max(1e-6, mu_incumbent));
        const double weight =
            point.denominator /
            (point.denominator + static_cast<double>(profile.d_min_train));
        const double var_candidate = std::max(
            kRatioVarianceFloor,
            profile.phi_over * point.denominator * candidate_p * (1.0 - candidate_p));
        const double var_incumbent = std::max(
            kRatioVarianceFloor,
            profile.phi_over * point.denominator * incumbent_p * (1.0 - incumbent_p));

        result.candidate_loss += weight * HuberLoss(
            (point.numerator - point.denominator * candidate_p) / std::sqrt(var_candidate));
        result.incumbent_loss += weight * HuberLoss(
            (point.numerator - point.denominator * incumbent_p) / std::sqrt(var_incumbent));
        ++result.validation_count;
    }
    return FinalizeValidationResult(result);
}

}  // namespace baseline
}  // namespace flowsql
