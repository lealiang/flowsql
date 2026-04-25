/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_TASK_RELATION_TASK_H_
#define _FLOWSQL_PLUGINS_BASELINE_TASK_RELATION_TASK_H_

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "baseline_task_base.h"
#include "plugins/baseline/detector/ratio_detector_core.h"
#include "plugins/baseline/detector/value_detector_core.h"
#include "plugins/baseline/fusion/fusion_types.h"
#include "plugins/baseline/model/formal_model_state.h"
#include "plugins/baseline/model/task_spec.h"
#include "plugins/baseline/relation/relation_basis.h"
#include "plugins/baseline/relation/relation_router.h"

namespace flowsql {
namespace baseline {

class TaskRegistry;
class RebuildQueue;
class RebuildTaskRuntime;
class KeyRiskFusion;
struct RebuildRequest;
struct RelationHistoryBinding;
struct RelationTaskTestAccess;

struct StoredRelationDetectorResult {
    bool available = false;
    int32_t status = 0;
    int64_t ts = 0;
    double raw_score = 0.0;
    double normalized_score = 0.0;
    double confidence = 0.0;
    uint32_t persistence = 0;
    BaselineDirection direction = BaselineDirection::kUnknown;
    BaselineSeverity severity = BaselineSeverity::kInfo;
    BaselineProvider provider = BaselineProvider::kNone;
    BaselineReasonCode reason_code = BaselineReasonCode::kUnknown;
    BaselineSourceKind baseline_source_kind = BaselineSourceKind::kNone;
    std::string baseline_source_key;
    std::string feature;
    std::string feature_type;
};

struct RelationRoutedFeatureRuntime {
    RelationRoutedFeatureSpec spec;
    StoredRelationDetectorResult last_detector_result;
    std::shared_ptr<ValueDetectorCore> value_core;
    std::shared_ptr<RatioDetectorCore> ratio_core;
};

struct RelationMetricRuntimeState {
    RelationServiceBasis service_basis;
    RelationEvalBasis eval_basis;
    RelationServiceBasis routed_basis_snapshot;
    bool routed_runtime_materialized = false;
    std::vector<RelationRoutedFeatureRuntime> routed_features;
};

struct RelationTaskKeyRuntimeState {
    uint64_t seen_block_count = 0;
    int64_t last_bucket_id = 0;
    std::unordered_map<std::string, RelationMetricRuntimeState> metrics_by_name;
    StoredFusionResult last_fusion_result;
    RebuildCandidateState candidate_state = RebuildCandidateState::kNone;
    RebuildSwitchState switch_state = RebuildSwitchState::kIdle;
    RebuildFailureReason failure_reason = RebuildFailureReason::kNone;
    std::string failure_reason_detail;
    RelationLineageCompatibility last_lineage_compatibility =
        RelationLineageCompatibility::kCompatible;
    double last_candidate_loss = 0.0;
    double last_incumbent_loss = 0.0;
    uint64_t validation_feature_count = 0;
    ReplayWindowSummary last_replay_window;
    ReplayWindowSummary last_train_window;
    ReplayWindowSummary last_holdout_window;
    RebuildStageTrace stage_trace;
};

class BaselineRelationTask final : public IBaselineRelationTask, public BaselineTaskBase {
 public:
    BaselineRelationTask(TaskRegistry* registry,
                         RebuildQueue* rebuild_queue,
                         std::string task_id,
                         const RelationTaskSpec& spec,
                         const RelationTaskClockSpec& clock_spec,
                         const std::optional<EventCalendarSpec>& event_calendar_spec,
                         IBaselineSourceResolver* source_resolver,
                         KeyRiskFusion* key_risk_fusion);

    const char* Id() const override;
    const char* Name() const override;
    BaselineTaskKind Kind() const override;
    const char* ConfigJson() const override;

    int QueryTaskSnapshotJson(std::string* out_json) const override;
    int QuerySeriesSnapshotJson(const BaselineStringRef& key,
                                std::string* out_json) const override;
    int RequestRebuild(const BaselineStringRef& key,
                       BaselineRebuildReason reason) override;
    int Close() override;

    int SetHistoryReader(IBaselineRelationHistoryReader* reader) override;
    int SubmitBlock(const RelationObservationBlock& block, FusionResult* out) override;

 protected:
    void OnClosingLocked() override;

 private:
    friend struct RelationTaskTestAccess;

    int ExecuteRebuild(const RebuildRequest& request);

    RelationTaskSpec spec_;
    RelationTaskClockSpec clock_spec_;
    std::optional<EventCalendarSpec> event_calendar_spec_;
    IBaselineSourceResolver* source_resolver_ = nullptr;
    mutable std::mutex runtime_mutex_;
    std::unordered_map<std::string, RelationTaskKeyRuntimeState> runtime_by_key_;
    size_t runtime_prune_cursor_ = 0;
    int64_t last_runtime_pruned_bucket_ = -1;
    uint64_t pruned_key_count_total_ = 0;
    std::shared_ptr<RelationHistoryBinding> history_binding_;
    std::shared_ptr<RebuildTaskRuntime> rebuild_runtime_;
    KeyRiskFusion* key_risk_fusion_ = nullptr;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_TASK_RELATION_TASK_H_
