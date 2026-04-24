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

#include "plugins/baseline/relation/relation_basis.h"
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

struct RelationMetricCandidateBuildResult {
    CandidateBuildStatus status = CandidateBuildStatus::kNone;
    RelationLineageCompatibility lineage_compatibility =
        RelationLineageCompatibility::kCompatible;
    RelationServiceBasis candidate_service_basis;
    RelationEvalBasis candidate_eval_basis;
};

class CandidateBuilder {
 public:
    static CandidateBuildStatus BuildValue(const ValueFeatureProfile& profile,
                                           const ValueReplaySeries& replay,
                                           uint64_t candidate_model_version,
                                           const BaselineTaskSpec* task_spec,
                                           int64_t delta,
                                           const std::string& tz,
                                           const EventCalendarSpec* event_calendar_spec,
                                           const CompiledEventCalendar* compiled_event_calendar,
                                           ValueCandidateBuildResult* out);
    static CandidateBuildStatus BuildRatio(const RatioFeatureProfile& profile,
                                           const RatioReplaySeries& replay,
                                           uint64_t candidate_model_version,
                                           const BaselineTaskSpec* task_spec,
                                           int64_t delta,
                                           const std::string& tz,
                                           const EventCalendarSpec* event_calendar_spec,
                                           const CompiledEventCalendar* compiled_event_calendar,
                                           RatioCandidateBuildResult* out);
    static CandidateBuildStatus BuildRelationMetricBases(
        const RelationBasisBuildInput& input,
        const RelationServiceBasis* incumbent_basis,
        const RelationTaskSpec& task_spec,
        RelationMetricCandidateBuildResult* out);
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_REBUILD_CANDIDATE_BUILDER_H_
