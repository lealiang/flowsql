/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_REBUILD_CANDIDATE_BUILDER_H_
#define _FLOWSQL_PLUGINS_BASELINE_REBUILD_CANDIDATE_BUILDER_H_

#include <cstdint>
#include <memory>

#include "formal_model_trainer.h"

namespace flowsql {
namespace baseline {

enum class CandidateBuildStatus : int32_t {
    kNone = 0,
    kTrained = 1,
    kEmpty = 2,
    kInsufficientTrainData = 3,
    kSolverUnavailable = 4,
    kTrainFailed = 5,
};

const char* CandidateBuildStatusName(CandidateBuildStatus status);

struct ValueCandidateBuildResult {
    CandidateBuildStatus status = CandidateBuildStatus::kNone;
    uint64_t candidate_model_version = 0;
    ReplayWindowSummary replay_window;
    ReplayWindowSummary train_window;
    ReplayWindowSummary holdout_window;
    std::shared_ptr<ValueFormalModel> candidate_model;
};

struct RatioCandidateBuildResult {
    CandidateBuildStatus status = CandidateBuildStatus::kNone;
    uint64_t candidate_model_version = 0;
    ReplayWindowSummary replay_window;
    ReplayWindowSummary train_window;
    ReplayWindowSummary holdout_window;
    std::shared_ptr<RatioFormalModel> candidate_model;
};

class CandidateBuilder {
 public:
    static CandidateBuildStatus BuildValue(const ValueFeatureProfile& profile,
                                           const ValueReplaySeries& replay,
                                           uint64_t candidate_model_version,
                                           const EventCalendarSpec* event_calendar_spec,
                                           ValueCandidateBuildResult* out);
    static CandidateBuildStatus BuildRatio(const RatioFeatureProfile& profile,
                                           const RatioReplaySeries& replay,
                                           uint64_t candidate_model_version,
                                           const EventCalendarSpec* event_calendar_spec,
                                           RatioCandidateBuildResult* out);
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_REBUILD_CANDIDATE_BUILDER_H_
