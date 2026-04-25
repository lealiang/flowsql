/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_MODEL_FORMAL_MODEL_STATE_H_
#define _FLOWSQL_PLUGINS_BASELINE_MODEL_FORMAL_MODEL_STATE_H_

#include <cstdint>
#include <string>

namespace flowsql {
namespace baseline {

struct ReplayWindowSummary {
    bool has_data = false;
    uint64_t observation_count = 0;
    int64_t first_bucket_id = 0;
    int64_t last_bucket_id = 0;
    int64_t request_bucket_start = 0;
    int64_t request_bucket_end = 0;
};

struct RebuildStageTrace {
    bool stage_seen_building = false;
    bool stage_seen_built = false;
    bool stage_seen_validating = false;
};

inline void ResetRebuildStageTrace(RebuildStageTrace* trace) {
    if (!trace) return;
    trace->stage_seen_building = false;
    trace->stage_seen_built = false;
    trace->stage_seen_validating = false;
}

inline void MarkRebuildStageBuilding(RebuildStageTrace* trace) {
    if (!trace) return;
    trace->stage_seen_building = true;
}

inline void MarkRebuildStageBuilt(RebuildStageTrace* trace) {
    if (!trace) return;
    trace->stage_seen_built = true;
}

inline void MarkRebuildStageValidating(RebuildStageTrace* trace) {
    if (!trace) return;
    trace->stage_seen_validating = true;
}

enum class RebuildCandidateState : uint8_t {
    kNone = 0,
    kBuilding = 1,
    kBuilt = 2,
    kValidating = 3,
    kAccepted = 4,
    kRejected = 5,
    kFailed = 6,
};

inline const char* RebuildCandidateStateName(RebuildCandidateState state) {
    switch (state) {
        case RebuildCandidateState::kBuilding:
            return "building";
        case RebuildCandidateState::kBuilt:
            return "built";
        case RebuildCandidateState::kValidating:
            return "validating";
        case RebuildCandidateState::kAccepted:
            return "accepted";
        case RebuildCandidateState::kRejected:
            return "rejected";
        case RebuildCandidateState::kFailed:
            return "failed";
        case RebuildCandidateState::kNone:
            break;
    }
    return "none";
}

enum class RebuildSwitchState : uint8_t {
    kIdle = 0,
    kShadowActive = 1,
    kRebuildPending = 2,
    kValidating = 3,
    kFormalApplied = 4,
    kRebuildBlocked = 5,
};

inline const char* RebuildSwitchStateName(RebuildSwitchState state) {
    switch (state) {
        case RebuildSwitchState::kShadowActive:
            return "shadow_active";
        case RebuildSwitchState::kRebuildPending:
            return "rebuild_pending";
        case RebuildSwitchState::kValidating:
            return "validating";
        case RebuildSwitchState::kFormalApplied:
            return "formal_applied";
        case RebuildSwitchState::kRebuildBlocked:
            return "rebuild_blocked";
        case RebuildSwitchState::kIdle:
            break;
    }
    return "idle";
}

enum class RebuildFailureReason : uint8_t {
    kNone = 0,
    kInsufficientData = 1,
    kUnavailable = 2,
    kReplayFailed = 3,
    kValidationFailed = 4,
    kBasisBuildFailed = 5,
    kTrainFailed = 6,
};

inline const char* RebuildFailureReasonName(RebuildFailureReason reason) {
    switch (reason) {
        case RebuildFailureReason::kInsufficientData:
            return "insufficient_data";
        case RebuildFailureReason::kUnavailable:
            return "unavailable";
        case RebuildFailureReason::kReplayFailed:
            return "replay_failed";
        case RebuildFailureReason::kValidationFailed:
            return "validation_failed";
        case RebuildFailureReason::kBasisBuildFailed:
            return "basis_build_failed";
        case RebuildFailureReason::kTrainFailed:
            return "train_failed";
        case RebuildFailureReason::kNone:
            break;
    }
    return "none";
}

struct FormalModelState {
    bool formal_ready = false;
    uint64_t formal_model_version = 0;
    std::string formal_model_kind = "none";
    uint64_t candidate_generation = 0;
    uint64_t candidate_model_version = 0;
    std::string candidate_model_kind = "none";
    RebuildCandidateState candidate_state = RebuildCandidateState::kNone;
    RebuildSwitchState switch_state = RebuildSwitchState::kIdle;
    RebuildFailureReason failure_reason = RebuildFailureReason::kNone;
    std::string failure_reason_detail;
    double last_candidate_loss = 0.0;
    double last_incumbent_loss = 0.0;
    uint64_t last_validation_count = 0;
    ReplayWindowSummary last_replay_window;
    ReplayWindowSummary last_train_window;
    ReplayWindowSummary last_holdout_window;
    RebuildStageTrace stage_trace;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_MODEL_FORMAL_MODEL_STATE_H_
