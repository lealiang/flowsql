/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/rolling/score_trust.h"

#include <algorithm>
#include <cmath>

namespace flowsql {
namespace baseline {
namespace {

bool ReadyEvidence(const RollingState& state, const BaselineRollingConfig& config) {
    return state.calibration_update_count >= config.score_ready_min_updates &&
           state.coverage_ewma >= config.calibration_coverage_floor &&
           state.tail3_ewma <= config.calibration_tail3_limit &&
           state.tail5_ewma <= config.calibration_tail5_limit &&
           MaturityAtLeast(state.maturity_status, RollingMaturityStatus::kLevelReady);
}

double LearningConfidenceForMaturity(RollingMaturityStatus status,
                                     const BaselineRollingConfig& config) {
    if (MaturityAtLeast(status, RollingMaturityStatus::kWeeklyReady)) {
        return config.confidence_ready_hint_cap;
    }
    if (MaturityAtLeast(status, RollingMaturityStatus::kLevelReady)) {
        return config.confidence_warming;
    }
    return config.confidence_cold;
}

bool AlertAllowedForMaturity(RollingMaturityStatus status,
                             double detection_z,
                             const BaselineRollingConfig& config) {
    if (MaturityAtLeast(status, RollingMaturityStatus::kDailyReady)) return true;
    return detection_z >= config.level_only_extreme_z;
}

double CombinedDriftEvidence(const RollingState& state) {
    return std::max(std::fabs(state.drift_evidence), std::fabs(state.level_shift_evidence));
}

void FillResult(const RollingState& state, ScoreTrustResult* out) {
    if (!out) return;
    out->score_confidence = state.score_confidence;
    out->effective_confidence = state.effective_confidence;
    out->reason = state.degradation_reason;
}

}  // namespace

BaselineStatus UpdateScoreTrust(const ObservedModelPoint& point,
                                double detection_z,
                                const BaselineRollingConfig& config,
                                RollingState* state,
                                ScoreTrustResult* out) {
    if (!state || !out || point.status != BaselineStatus::kOk || !std::isfinite(detection_z)) {
        return BaselineStatus::kInvalidArgument;
    }

    ScoreTrustResult result;
    state->learning_confidence = LearningConfidenceForMaturity(state->maturity_status, config);

    if (!point.can_score) {
        state->score_trust_status = ScoreTrustStatus::kScoreUntrusted;
        state->score_confidence = 0.0;
        state->effective_confidence = 0.0;
        state->degradation_reason = "score_unavailable";
        result.can_alert = false;
        FillResult(*state, &result);
        *out = result;
        return BaselineStatus::kOk;
    }

    if (CombinedDriftEvidence(*state) >= config.score_drift_degrade_start) {
        state->score_trust_status = ScoreTrustStatus::kDriftLearning;
        state->score_confidence = 0.0;
        state->effective_confidence = 0.0;
        state->stable_score_count = 0;
        state->last_degradation_bucket = point.bucket_id;
        state->degradation_reason =
            std::fabs(state->level_shift_evidence) > std::fabs(state->drift_evidence)
                ? "level_shift_learning"
                : "drift_learning";
        result.can_alert = false;
        FillResult(*state, &result);
        *out = result;
        return BaselineStatus::kOk;
    }

    if (state->score_trust_status == ScoreTrustStatus::kDriftLearning) {
        state->score_trust_status = ScoreTrustStatus::kRecalibrating;
        state->calibration_status = RollingCalibrationStatus::kRecalibrating;
        state->stable_score_count = 0;
        state->score_confidence = 0.0;
        state->effective_confidence = 0.0;
        state->degradation_reason = "recalibrating";
        result.can_alert = false;
        FillResult(*state, &result);
        *out = result;
        return BaselineStatus::kOk;
    }

    if (state->score_trust_status == ScoreTrustStatus::kRecalibrating) {
        state->stable_score_count += 1;
        if (state->stable_score_count < config.score_recovery_min_updates) {
            state->score_confidence = 0.0;
            state->effective_confidence = 0.0;
            state->degradation_reason = "recalibrating";
            result.can_alert = false;
            FillResult(*state, &result);
            *out = result;
            return BaselineStatus::kOk;
        }
    }

    if (!MaturityAtLeast(state->maturity_status, RollingMaturityStatus::kLevelReady) ||
        state->calibration_update_count < config.score_warming_min_updates) {
        state->score_trust_status = ScoreTrustStatus::kScoreUntrusted;
        state->score_confidence = 0.0;
        state->effective_confidence = 0.0;
        state->degradation_reason = "score_warming";
        result.can_alert = false;
        FillResult(*state, &result);
        *out = result;
        return BaselineStatus::kOk;
    }

    if (ReadyEvidence(*state, config)) {
        state->score_trust_status = ScoreTrustStatus::kScoreReady;
        state->score_confidence = config.confidence_ready_hint_cap;
        state->score_ready_count += 1;
        state->degradation_reason.clear();
    } else {
        state->score_trust_status = ScoreTrustStatus::kScoreWarming;
        state->score_confidence = config.confidence_warming;
        state->degradation_reason = "calibration_warming";
    }
    state->effective_confidence =
        std::min(state->learning_confidence, state->score_confidence);
    result.can_alert = state->score_trust_status == ScoreTrustStatus::kScoreReady &&
                       AlertAllowedForMaturity(state->maturity_status, detection_z, config);
    FillResult(*state, &result);
    *out = result;
    return BaselineStatus::kOk;
}

}  // namespace baseline
}  // namespace flowsql
