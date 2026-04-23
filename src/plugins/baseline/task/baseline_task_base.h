/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_TASK_BASELINE_TASK_BASE_H_
#define _FLOWSQL_PLUGINS_BASELINE_TASK_BASELINE_TASK_BASE_H_

#include <framework/interfaces/ibaseline_service.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "plugins/baseline/model/series_store.h"
#include "plugins/baseline/model/task_spec.h"
#include "plugins/baseline/model/formal_model_state.h"
#include "plugins/baseline/relation/relation_basis.h"
#include "plugins/baseline/relation/relation_router.h"
#include "ratio_task.h"

namespace flowsql {
namespace baseline {

class TaskRegistry;
class RebuildQueue;
class RebuildTaskRuntime;
class ValueDetectorCore;
class RatioDetectorCore;
struct RebuildRequest;
struct ValueHistoryBinding;
struct RatioHistoryBinding;
struct RelationHistoryBinding;

struct RelationTaskKeyRuntimeState {
    uint64_t seen_block_count = 0;
    std::unordered_map<std::string, RelationServiceBasis> service_basis_by_metric;
    std::string candidate_state = "none";
    std::string switch_state = "none";
    RelationLineageCompatibility last_lineage_compatibility =
        RelationLineageCompatibility::kCompatible;
    double last_candidate_loss = 0.0;
    double last_incumbent_loss = 0.0;
    uint64_t validation_feature_count = 0;
    ReplayWindowSummary last_replay_window;
    ReplayWindowSummary last_train_window;
    ReplayWindowSummary last_holdout_window;
};

class BaselineTaskBase : public std::enable_shared_from_this<BaselineTaskBase> {
 public:
    BaselineTaskBase(TaskRegistry* registry,
                     RebuildQueue* rebuild_queue,
                     std::string task_id,
                     BaselineTaskKind kind,
                     std::string task_name,
                     std::string config_json);
    virtual ~BaselineTaskBase() = default;

    const char* Id() const;
    const char* Name() const;
    BaselineTaskKind Kind() const;
    const char* ConfigJson() const;
    const std::string& TaskId() const;

    int QueryTaskSnapshotJson(std::string* out_json) const;
    int QuerySeriesSnapshotJson(const BaselineStringRef& key,
                                std::string* out_json) const;
    int RequestRebuild(const BaselineStringRef& key,
                       BaselineRebuildReason reason);
    int Close();

 protected:
    int EnsureOpenLocked() const;
    static std::string CopyStringRef(const BaselineStringRef& ref);
    virtual void OnClosingLocked();

    mutable std::mutex mutex_;
    RebuildQueue* rebuild_queue_ = nullptr;

 private:
    static const char* KindName(BaselineTaskKind kind);

    TaskRegistry* registry_ = nullptr;
    std::string task_id_;
    BaselineTaskKind kind_ = BaselineTaskKind::kValue;
    std::string task_name_;
    std::string config_json_;
    bool closed_ = false;
};

class BaselineValueTask final : public IBaselineValueTask, public BaselineTaskBase {
 public:
    BaselineValueTask(TaskRegistry* registry,
                      RebuildQueue* rebuild_queue,
                      std::string task_id,
                      const BaselineTaskSpec& spec);

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

    int SetHistoryReader(IBaselineValueHistoryReader* reader) override;
    int SubmitObservation(const ValueObservation& obs,
                          DetectorResult* out) override;

 protected:
    void OnClosingLocked() override;

 private:
    int ExecuteRebuild(const RebuildRequest& request);

    BaselineTaskSpec spec_;
    std::shared_ptr<ValueDetectorCore> core_;
    std::shared_ptr<ValueHistoryBinding> history_binding_;
    std::shared_ptr<RebuildTaskRuntime> rebuild_runtime_;
};

class BaselineRatioTask final : public IBaselineRatioTask, public BaselineTaskBase {
 public:
    BaselineRatioTask(TaskRegistry* registry,
                      RebuildQueue* rebuild_queue,
                      std::string task_id,
                      const BaselineTaskSpec& spec);

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

    int SetHistoryReader(IBaselineRatioHistoryReader* reader) override;
    int SubmitObservation(const RatioObservation& obs,
                          DetectorResult* out) override;

 protected:
    void OnClosingLocked() override;

 private:
    int ExecuteRebuild(const RebuildRequest& request);

    BaselineTaskSpec spec_;
    std::shared_ptr<RatioDetectorCore> core_;
    std::shared_ptr<RatioHistoryBinding> history_binding_;
    std::shared_ptr<RebuildTaskRuntime> rebuild_runtime_;
};

class BaselineRelationTask final : public IBaselineRelationTask, public BaselineTaskBase {
 public:
    BaselineRelationTask(TaskRegistry* registry,
                         RebuildQueue* rebuild_queue,
                         std::string task_id,
                         const RelationTaskSpec& spec);

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
    int SubmitBlock(const RelationObservationBlock& block,
                    DetectorResult* out) override;

 protected:
    void OnClosingLocked() override;

 private:
    int ExecuteRebuild(const RebuildRequest& request);
    void EnsureRoutedDetectors();

    RelationTaskSpec spec_;
    mutable std::mutex runtime_mutex_;
    std::unordered_map<std::string, RelationTaskKeyRuntimeState> runtime_by_key_;
    std::vector<RelationRoutedFeatureSpec> routed_feature_specs_;
    std::unordered_map<std::string, std::shared_ptr<ValueDetectorCore>>
        value_cores_by_routed_feature_;
    std::unordered_map<std::string, std::shared_ptr<RatioDetectorCore>>
        ratio_cores_by_routed_feature_;
    std::shared_ptr<RelationHistoryBinding> history_binding_;
    std::shared_ptr<RebuildTaskRuntime> rebuild_runtime_;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_TASK_BASELINE_TASK_BASE_H_
