/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_TASK_REBUILD_OUTCOME_HELPER_H_
#define _FLOWSQL_PLUGINS_BASELINE_TASK_REBUILD_OUTCOME_HELPER_H_

#include "plugins/baseline/model/formal_model_state.h"
#include "plugins/baseline/rebuild/candidate_builder.h"
#include "plugins/baseline/rebuild/candidate_validator.h"

namespace flowsql {
namespace baseline {

template <typename TResult>
inline void SetRebuildFailureOutcome(TResult* out,
                                     RebuildFailureReason reason,
                                     const char* detail,
                                     RebuildSwitchState switch_state = RebuildSwitchState::kIdle) {
    if (!out) return;
    out->candidate_state = RebuildCandidateState::kFailed;
    out->switch_state = switch_state;
    out->failure_reason = reason;
    out->failure_reason_detail = detail ? detail : "";
}

template <typename TResult>
inline void SetRebuildRejectedOutcome(TResult* out, const char* detail) {
    if (!out) return;
    out->candidate_state = RebuildCandidateState::kRejected;
    out->switch_state = RebuildSwitchState::kIdle;
    out->failure_reason = RebuildFailureReason::kValidationFailed;
    out->failure_reason_detail = detail ? detail : "";
}

template <typename TResult>
inline void SetRebuildAcceptedOutcome(TResult* out) {
    if (!out) return;
    out->candidate_state = RebuildCandidateState::kAccepted;
    out->switch_state = RebuildSwitchState::kFormalApplied;
    out->failure_reason = RebuildFailureReason::kNone;
    out->failure_reason_detail.clear();
}

template <typename TResult>
inline void ApplyBuildFailureOutcome(CandidateBuildStatus status, TResult* out) {
    switch (status) {
        case CandidateBuildStatus::kEmpty:
        case CandidateBuildStatus::kInsufficientTrainData:
            SetRebuildFailureOutcome(out,
                                     RebuildFailureReason::kInsufficientData,
                                     CandidateBuildStatusName(status));
            return;
        case CandidateBuildStatus::kSolverUnavailable:
            SetRebuildFailureOutcome(out,
                                     RebuildFailureReason::kUnavailable,
                                     CandidateBuildStatusName(status));
            return;
        case CandidateBuildStatus::kTrainFailed:
            SetRebuildFailureOutcome(out,
                                     RebuildFailureReason::kTrainFailed,
                                     CandidateBuildStatusName(status));
            return;
        case CandidateBuildStatus::kNone:
        case CandidateBuildStatus::kTrained:
            break;
    }
    SetRebuildFailureOutcome(out, RebuildFailureReason::kTrainFailed, "unexpected_build_status");
}

template <typename TResult>
inline void ApplyValidationFailureOutcome(
    CandidateValidationStatus status,
    bool full_model_train_failed,
    TResult* out,
    RebuildSwitchState switch_state = RebuildSwitchState::kIdle) {
    if (full_model_train_failed) {
        SetRebuildFailureOutcome(out,
                                 RebuildFailureReason::kTrainFailed,
                                 "full_model_train_failed",
                                 switch_state);
        return;
    }

    switch (status) {
        case CandidateValidationStatus::kFailed:
            SetRebuildRejectedOutcome(out, CandidateValidationStatusName(status));
            return;
        case CandidateValidationStatus::kInsufficientHoldout:
            SetRebuildFailureOutcome(out,
                                     RebuildFailureReason::kInsufficientData,
                                     CandidateValidationStatusName(status),
                                     switch_state);
            return;
        case CandidateValidationStatus::kUnavailableIncumbent:
            SetRebuildFailureOutcome(out,
                                     RebuildFailureReason::kUnavailable,
                                     CandidateValidationStatusName(status),
                                     switch_state);
            return;
        case CandidateValidationStatus::kNone:
            SetRebuildFailureOutcome(
                out, RebuildFailureReason::kTrainFailed, "validation_not_run", switch_state);
            return;
        case CandidateValidationStatus::kBypassNoIncumbent:
        case CandidateValidationStatus::kPassed:
            break;
    }
    SetRebuildFailureOutcome(
        out, RebuildFailureReason::kTrainFailed, "unexpected_validation_status", switch_state);
}

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_TASK_REBUILD_OUTCOME_HELPER_H_
