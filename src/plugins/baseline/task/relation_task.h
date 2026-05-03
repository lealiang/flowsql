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
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "baseline_task_base.h"
#include "bootstrap_task_store.h"
#include "plugins/baseline/model/event_calendar_matcher.h"
#include "plugins/baseline/model/task_spec.h"
#include "plugins/baseline/relation/relation_basis_state.h"
#include "plugins/baseline/relation/relation_fusion.h"
#include "plugins/baseline/relation/relation_summary.h"
#include "plugins/baseline/rolling/rolling_config.h"
#include "plugins/baseline/rolling/rolling_task_runner.h"

namespace flowsql {
namespace baseline {

class TaskRegistry;

class BaselineRelationTask final : public IBaselineRelationTask, public BaselineTaskBase {
 public:
    BaselineRelationTask(TaskRegistry* registry,
                         std::string task_id,
                         std::string task_name,
                         std::string config_content,
                         RelationTaskCreateSpec spec,
                         std::shared_ptr<const CompiledEventCalendar> compiled_event_calendar);

    const char* Id() const override;
    const char* Name() const override;
    BaselineTaskKind Kind() const override;

    BaselineSerializationResult ExportConfig(
        BaselineSerializationFormat format) const override;
    BaselineSerializationResult QueryTaskSnapshot(
        BaselineSerializationFormat format) const override;
    BaselineSerializationResult QuerySeriesSnapshot(
        std::string_view series_key,
        BaselineSerializationFormat format) const override;
    BaselineStatus Close() override;

    RelationRollingResult SubmitObservation(
        const RelationRollingObservation& obs,
        const RelationRollingSubmitOptions& options) override;
    RollingPrediction PredictRoutedSummary(
        const RelationRoutedSummaryQuery& query,
        int64_t bucket_id) const override;
    BaselineSerializationResult QueryRoutedSummarySnapshot(
        const RelationRoutedSummaryQuery& query,
        BaselineSerializationFormat format) const override;

    BootstrapTrainResult Bootstrap(const RelationBootstrapInput& input) override;
    BaselineSerializationResult ExportBootstrapArtifact(
        BaselineSerializationFormat format) const override;
    BaselineStatus LoadBootstrapArtifact(
        std::string_view content,
        BaselineSerializationFormat format) override;
    BaselineSerializationResult ExportBootstrapSeed(
        BaselineSerializationFormat format) const override;
    BaselineSerializationResult QueryBootstrapBasis(
        BaselineSerializationFormat format) const override;

 private:
    struct RelationRoutedRuntimeShard {
        BootstrapSeedStore routed_seeds_by_series;
        std::unordered_map<std::string, BaselineTaskSpec> routed_specs_by_series;
        RollingStateMap routed_rolling_states;
    };

    using RelationBasisStateMap =
        std::unordered_map<std::string, RelationBasisRuntimeState>;

    std::size_t RoutedShardIndex(std::string_view routed_series_key) const;
    void RebuildRuntimeFromRelationSeeds();
    RelationBasisRuntimeConfig MakeBasisRuntimeConfig() const;
    RelationFusionRuntimeConfig MakeFusionRuntimeConfig() const;
    bool IsFusionStateExpired(const RelationFusionRuntimeState& state) const;
    void MaybeCleanupFusionStates(std::string_view current_source,
                                  bool make_room_for_current_source);
    void ResetFusionCleanupRuntime();

    RelationTaskCreateSpec spec_;
    std::shared_ptr<const CompiledEventCalendar> compiled_event_calendar_;
    BootstrapArtifactStore artifacts_by_series_;
    BootstrapSeedStore seeds_by_series_;
    BootstrapEngine bootstrap_engine_;
    BaselineRelationRollingConfig relation_rolling_config_;
    std::size_t runtime_shard_count_ = 16;
    std::vector<std::unique_ptr<RelationRoutedRuntimeShard>> routed_shards_;
    RelationBasisStateMap basis_states_;
    std::unordered_map<std::string, RelationFusionRuntimeState> fusion_states_;
    uint64_t fusion_update_seq_ = 0;
    std::size_t fusion_cleanup_bucket_cursor_ = 0;
    uint64_t fusion_state_evicted_total_ = 0;
    uint64_t fusion_state_evicted_ttl_total_ = 0;
    uint64_t fusion_state_evicted_capacity_total_ = 0;
    uint64_t fusion_persistence_key_evicted_total_ = 0;
    uint64_t fusion_cleanup_last_scan_count_ = 0;
    uint64_t fusion_cleanup_last_evicted_count_ = 0;
    int64_t fusion_cleanup_watermark_bucket_id_ = 0;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_TASK_RELATION_TASK_H_
