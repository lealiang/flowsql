/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_REBUILD_CANDIDATE_VALIDATOR_H_
#define _FLOWSQL_PLUGINS_BASELINE_REBUILD_CANDIDATE_VALIDATOR_H_

#include <cstdint>

#include "plugins/baseline/detector/ratio_detector_core.h"
#include "plugins/baseline/detector/value_detector_core.h"
#include "plugins/baseline/model/shadow_state.h"
#include "plugins/baseline/rebuild/replay_runner.h"

namespace flowsql {
namespace baseline {

enum class CandidateValidationStatus : int32_t {
    kNone = 0,
    kBypassNoIncumbent = 1,
    kPassed = 2,
    kFailed = 3,
    kInsufficientHoldout = 4,
    kUnavailableIncumbent = 5,
};

const char* CandidateValidationStatusName(CandidateValidationStatus status);

struct CandidateValidationResult {
    CandidateValidationStatus status = CandidateValidationStatus::kNone;
    bool pass = false;
    double candidate_loss = 0.0;
    double incumbent_loss = 0.0;
    uint64_t validation_count = 0;
};

class CandidateValidator {
 public:
    static CandidateValidationResult ValidateValue(
        const ValueFeatureProfile& profile,
        const ValueReplaySeries& replay,
        const ReplayWindowSummary& holdout_window,
        const ValueFormalModel* candidate_model,
        const ValueFormalModel* incumbent_formal_model,
        const ValueShadowState* incumbent_shadow_state);

    static CandidateValidationResult ValidateRatio(
        const RatioFeatureProfile& profile,
        const RatioReplaySeries& replay,
        const ReplayWindowSummary& holdout_window,
        const RatioFormalModel* candidate_model,
        const RatioFormalModel* incumbent_formal_model,
        const RatioShadowState* incumbent_shadow_state);

    static CandidateValidationResult ValidateRelationAggregate(
        double candidate_loss_sum,
        double incumbent_loss_sum,
        uint64_t validation_feature_count);
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_REBUILD_CANDIDATE_VALIDATOR_H_
