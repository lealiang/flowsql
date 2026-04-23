/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "baseline_task_base.h"

#include <common/error_code.h>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "plugins/baseline/detector/ratio_detector_core.h"
#include "plugins/baseline/detector/value_detector_core.h"
#include "plugins/baseline/relation/relation_summary_extractor.h"
#include "plugins/baseline/rebuild/candidate_builder.h"
#include "plugins/baseline/rebuild/candidate_validator.h"
#include "plugins/baseline/rebuild/formal_model_trainer.h"
#include "plugins/baseline/rebuild/rebuild_queue.h"
#include "plugins/baseline/rebuild/rebuild_worker.h"
#include "task_registry.h"

namespace flowsql {
namespace baseline {

struct RelationHistoryBinding {
    mutable std::mutex mutex;
    IBaselineRelationHistoryReader* reader = nullptr;

    bool HasReader() const {
        std::lock_guard<std::mutex> lock(mutex);
        return reader != nullptr;
    }
};

namespace {

void FillUnavailableResult(DetectorResult* out) {
    if (!out) return;
    *out = DetectorResult{};
    out->status = error::UNAVAILABLE;
}

void FillBadRequestResult(DetectorResult* out) {
    if (!out) return;
    *out = DetectorResult{};
    out->status = error::BAD_REQUEST;
}

std::string CopyKey(const BaselineStringRef& key) {
    if (!key.data || key.size == 0) return "";
    return std::string(key.data, key.size);
}

int ValidateRelationBlock(const RelationTaskSpec& spec,
                          const RelationObservationBlock& block) {
    if (!block.key.data || block.key.size == 0) return error::BAD_REQUEST;
    if (block.bucket_id < 0) return error::BAD_REQUEST;
    if (block.metric_count != spec.metrics.size()) return error::BAD_REQUEST;
    if (block.nnz > 0 && !block.group_idx) return error::BAD_REQUEST;
    if (block.metric_count > 0 && !block.metrics) return error::BAD_REQUEST;

    for (uint32_t i = 0; i < block.metric_count; ++i) {
        const auto& metric = block.metrics[i];
        if (metric.total < 0.0) return error::BAD_REQUEST;
        if (block.nnz > 0 && !metric.values) return error::BAD_REQUEST;
    }
    return error::OK;
}

RelationBasisBuildInput MakeBootstrapBasisInput(const RelationTaskSpec& spec,
                                                const RelationObservationBlock& block,
                                                uint32_t metric_index) {
    RelationBasisBuildInput input;
    input.basis_version = 1;
    input.feature_base = spec.feature_base;
    input.metric_name = spec.metrics[metric_index];
    input.group_space_id = spec.group_space_id;
    input.group_space_version = spec.group_space_version;
    input.support_policy = spec.support_policy;
    input.summary_policy = spec.summary_policy;
    input.valid_bucket_count = 1;
    input.group_stats.reserve(block.nnz);
    const RelationMetricBlock& metric = block.metrics[metric_index];
    for (uint32_t i = 0; i < block.nnz; ++i) {
        const double mass = metric.values[i];
        if (mass <= 0.0) continue;
        input.group_stats.push_back(RelationGroupHistoryStat{block.group_idx[i], mass, 1});
    }
    return input;
}

void MergeFeatureResult(const DetectorResult& candidate,
                        bool* has_serviceable,
                        DetectorResult* aggregate) {
    if (!has_serviceable || !aggregate) return;
    if (candidate.status != error::OK) return;

    if (!*has_serviceable || candidate.normalized_score > aggregate->normalized_score) {
        *aggregate = candidate;
        *has_serviceable = true;
    } else {
        aggregate->flags |= candidate.flags;
    }
}

void MergeRebuildIntent(const DetectorSubmitOutput& submit_output,
                        bool* required,
                        BaselineRebuildReason* reason,
                        int64_t* bucket_start_hint,
                        int64_t* bucket_end) {
    if (!required || !reason || !bucket_start_hint || !bucket_end) return;
    if (!submit_output.rebuild_intent.required) return;

    if (!*required) {
        *required = true;
        *reason = submit_output.rebuild_intent.reason;
        *bucket_start_hint = submit_output.rebuild_intent.rebuild_start_hint;
        *bucket_end = submit_output.rebuild_intent.bucket_end;
        return;
    }

    *bucket_start_hint =
        std::min(*bucket_start_hint, submit_output.rebuild_intent.rebuild_start_hint);
    *bucket_end = std::max(*bucket_end, submit_output.rebuild_intent.bucket_end);
}

struct OwnedRelationMetricBlock {
    double total = 0.0;
    uint32_t active_count = 0;
    std::vector<double> values;
};

struct OwnedRelationObservationBlock {
    int64_t bucket_id = 0;
    std::vector<uint32_t> group_idx;
    std::vector<OwnedRelationMetricBlock> metrics;
};

struct RelationApplyPlan {
    const RelationRoutedFeatureSpec* feature_spec = nullptr;
    uint64_t model_version = 0;
    double candidate_loss = 0.0;
    double incumbent_loss = 0.0;
    uint64_t validation_count = 0;
    ReplayWindowSummary replay_window;
    ReplayWindowSummary train_window;
    ReplayWindowSummary holdout_window;
    std::shared_ptr<ValueFormalModel> value_full_model;
    std::shared_ptr<RatioFormalModel> ratio_full_model;
};

struct RelationTaskRebuildResult {
    std::string candidate_state = "none";
    std::string switch_state = "none";
    RelationLineageCompatibility lineage_compatibility =
        RelationLineageCompatibility::kIdentical;
    double candidate_loss = 0.0;
    double incumbent_loss = 0.0;
    uint64_t validation_feature_count = 0;
    ReplayWindowSummary replay_window;
    ReplayWindowSummary train_window;
    ReplayWindowSummary holdout_window;
    std::unordered_map<std::string, RelationServiceBasis> service_basis_by_metric;
    std::vector<RelationApplyPlan> apply_plans;
};

constexpr std::size_t kRelationMinReplayForHoldout = 3;
constexpr std::size_t kRelationSwitchValidationTail = 16;

std::size_t DecideRelationHoldoutCount(std::size_t total_count) {
    if (total_count < kRelationMinReplayForHoldout) return 0;
    return std::min(kRelationSwitchValidationTail, total_count / 2);
}

ReplayWindowSummary BuildRelationWindowSummary(
    const std::vector<OwnedRelationObservationBlock>& blocks,
    std::size_t begin,
    std::size_t end,
    int64_t request_bucket_start,
    int64_t request_bucket_end) {
    ReplayWindowSummary summary;
    summary.request_bucket_start = request_bucket_start;
    summary.request_bucket_end = request_bucket_end;
    if (begin >= end || end > blocks.size()) return summary;

    summary.has_data = true;
    summary.observation_count = static_cast<uint64_t>(end - begin);
    summary.first_bucket_id = blocks[begin].bucket_id;
    summary.last_bucket_id = blocks[end - 1].bucket_id;
    return summary;
}

template <typename TReplaySeries>
ReplayWindowSummary BuildReplayWindowSummary(const TReplaySeries& replay,
                                            std::size_t begin,
                                            std::size_t end) {
    ReplayWindowSummary summary;
    summary.request_bucket_start = replay.window.request_bucket_start;
    summary.request_bucket_end = replay.window.request_bucket_end;
    if (begin >= end || end > replay.points.size()) return summary;

    summary.has_data = true;
    summary.observation_count = static_cast<uint64_t>(end - begin);
    summary.first_bucket_id = replay.points[begin].bucket_id;
    summary.last_bucket_id = replay.points[end - 1].bucket_id;
    return summary;
}

std::shared_ptr<ValueFormalModel> TrainFullValueModel(
    const ValueFeatureProfile& profile,
    const ValueReplaySeries& replay,
    uint64_t model_version) {
    ValueFormalTrainResult train_result;
    const ValueFormalTrainInput input{
        &profile,
        &replay,
        replay.points.size(),
        model_version,
        0,
        replay.window,
        nullptr};
    if (FormalModelTrainer::TrainValue(input, &train_result) !=
        FormalTrainFailureCode::kNone) {
        return nullptr;
    }
    return train_result.model;
}

std::shared_ptr<RatioFormalModel> TrainFullRatioModel(
    const RatioFeatureProfile& profile,
    const RatioReplaySeries& replay,
    uint64_t model_version) {
    RatioFormalTrainResult train_result;
    const RatioFormalTrainInput input{
        &profile,
        &replay,
        replay.points.size(),
        model_version,
        0,
        replay.window,
        nullptr};
    if (FormalModelTrainer::TrainRatio(input, &train_result) !=
        FormalTrainFailureCode::kNone) {
        return nullptr;
    }
    return train_result.model;
}

int CopyRelationBlock(const RelationTaskSpec& spec,
                      const std::string& expected_key,
                      const RelationObservationBlock& block,
                      OwnedRelationObservationBlock* out_block) {
    if (!out_block) return error::BAD_REQUEST;

    const int rc = ValidateRelationBlock(spec, block);
    if (rc != error::OK) return rc;
    if (CopyKey(block.key) != expected_key) return error::BAD_REQUEST;

    OwnedRelationObservationBlock owned;
    owned.bucket_id = block.bucket_id;
    if (block.nnz > 0) {
        owned.group_idx.assign(block.group_idx, block.group_idx + block.nnz);
    }
    owned.metrics.reserve(block.metric_count);
    for (uint32_t i = 0; i < block.metric_count; ++i) {
        OwnedRelationMetricBlock metric;
        metric.total = block.metrics[i].total;
        metric.active_count = block.metrics[i].active_count;
        if (block.nnz > 0) {
            metric.values.assign(block.metrics[i].values, block.metrics[i].values + block.nnz);
        }
        owned.metrics.push_back(std::move(metric));
    }

    *out_block = std::move(owned);
    return error::OK;
}

RelationObservationBlock BuildRelationBlockView(
    const std::string& key,
    const OwnedRelationObservationBlock& owned_block,
    std::vector<RelationMetricBlock>* metric_views) {
    if (metric_views) {
        metric_views->clear();
        metric_views->reserve(owned_block.metrics.size());
        for (const auto& metric : owned_block.metrics) {
            metric_views->push_back(RelationMetricBlock{
                metric.total,
                metric.active_count,
                metric.values.empty() ? nullptr : metric.values.data()});
        }
    }

    return RelationObservationBlock{
        BaselineStringRef{key.c_str(), static_cast<uint32_t>(key.size())},
        owned_block.bucket_id,
        static_cast<uint32_t>(owned_block.group_idx.size()),
        owned_block.group_idx.empty() ? nullptr : owned_block.group_idx.data(),
        static_cast<uint32_t>(owned_block.metrics.size()),
        (metric_views && !metric_views->empty()) ? metric_views->data() : nullptr};
}

RelationBasisBuildInput BuildRelationBasisInput(
    const RelationTaskSpec& spec,
    const std::vector<OwnedRelationObservationBlock>& blocks,
    uint32_t metric_index,
    std::size_t block_limit,
    uint64_t basis_version) {
    RelationBasisBuildInput input;
    input.basis_version = basis_version;
    input.feature_base = spec.feature_base;
    input.metric_name = spec.metrics[metric_index];
    input.group_space_id = spec.group_space_id;
    input.group_space_version = spec.group_space_version;
    input.support_policy = spec.support_policy;
    input.summary_policy = spec.summary_policy;

    std::unordered_map<uint32_t, RelationGroupHistoryStat> stats_by_group;
    const std::size_t limit = std::min(block_limit, blocks.size());
    for (std::size_t block_index = 0; block_index < limit; ++block_index) {
        if (metric_index >= blocks[block_index].metrics.size()) continue;
        const auto& metric = blocks[block_index].metrics[metric_index];
        if (metric.total <= 0.0) continue;

        ++input.valid_bucket_count;
        for (std::size_t i = 0; i < blocks[block_index].group_idx.size(); ++i) {
            if (i >= metric.values.size() || metric.values[i] <= 0.0) continue;
            auto& stat = stats_by_group[blocks[block_index].group_idx[i]];
            stat.group_idx = blocks[block_index].group_idx[i];
            stat.hist_mass += metric.values[i];
            stat.active_bucket_count += 1;
        }
    }

    input.group_stats.reserve(stats_by_group.size());
    for (const auto& entry : stats_by_group) {
        input.group_stats.push_back(entry.second);
    }
    return input;
}

int BuildRelationMetricSummaries(
    const std::string& key,
    const std::vector<OwnedRelationObservationBlock>& blocks,
    uint32_t metric_index,
    const RelationServiceBasis& basis,
    std::vector<RelationMetricSummary>* out_summaries) {
    if (!out_summaries) return error::BAD_REQUEST;
    out_summaries->clear();
    out_summaries->reserve(blocks.size());

    std::vector<RelationMetricBlock> metric_views;
    for (const auto& block : blocks) {
        RelationMetricSummary summary;
        const auto block_view = BuildRelationBlockView(key, block, &metric_views);
        const int rc = RelationSummaryExtractor::ExtractMetricSummary(
            block_view, metric_index, basis, &summary);
        if (rc != error::OK) return rc;
        out_summaries->push_back(std::move(summary));
    }
    return error::OK;
}

int BuildRelationValueReplay(
    const RelationRoutedFeatureSpec& feature_spec,
    const std::string& key,
    const std::vector<OwnedRelationObservationBlock>& blocks,
    const std::vector<RelationMetricSummary>& summaries,
    int64_t request_bucket_start,
    int64_t request_bucket_end,
    ValueReplaySeries* out_replay) {
    if (!out_replay || blocks.size() != summaries.size()) return error::BAD_REQUEST;

    ValueReplayRunner runner(key);
    const BaselineStringRef key_ref{key.c_str(), static_cast<uint32_t>(key.size())};
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        ValueObservation observation;
        if (!RelationRouter::BuildValueObservation(
                feature_spec, key_ref, blocks[i].bucket_id, summaries[i], &observation)) {
            continue;
        }
        const int rc = runner.Push(observation);
        if (rc != error::OK) return rc;
    }
    runner.Finalize(request_bucket_start, request_bucket_end, out_replay);
    return error::OK;
}

int BuildRelationRatioReplay(
    const RelationRoutedFeatureSpec& feature_spec,
    const std::string& key,
    const std::vector<OwnedRelationObservationBlock>& blocks,
    const std::vector<RelationMetricSummary>& summaries,
    int64_t request_bucket_start,
    int64_t request_bucket_end,
    RatioReplaySeries* out_replay) {
    if (!out_replay || blocks.size() != summaries.size()) return error::BAD_REQUEST;

    RatioReplayRunner runner(key);
    const BaselineStringRef key_ref{key.c_str(), static_cast<uint32_t>(key.size())};
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        RatioObservation observation;
        if (!RelationRouter::BuildRatioObservation(
                feature_spec, key_ref, blocks[i].bucket_id, summaries[i], &observation)) {
            continue;
        }
        const int rc = runner.Push(observation);
        if (rc != error::OK) return rc;
    }
    runner.Finalize(request_bucket_start, request_bucket_end, out_replay);
    return error::OK;
}

RelationLineageCompatibility MergeLineageCompatibility(
    RelationLineageCompatibility lhs,
    RelationLineageCompatibility rhs) {
    return static_cast<int32_t>(lhs) >= static_cast<int32_t>(rhs) ? lhs : rhs;
}

}  // namespace

BaselineTaskBase::BaselineTaskBase(TaskRegistry* registry,
                                   RebuildQueue* rebuild_queue,
                                   std::string task_id,
                                   BaselineTaskKind kind,
                                   std::string task_name,
                                   std::string config_json)
    : registry_(registry),
      rebuild_queue_(rebuild_queue),
      task_id_(std::move(task_id)),
      kind_(kind),
      task_name_(std::move(task_name)),
      config_json_(std::move(config_json)) {}

const char* BaselineTaskBase::Id() const {
    return task_id_.c_str();
}

const char* BaselineTaskBase::Name() const {
    return task_name_.c_str();
}

BaselineTaskKind BaselineTaskBase::Kind() const {
    return kind_;
}

const char* BaselineTaskBase::ConfigJson() const {
    return config_json_.c_str();
}

const std::string& BaselineTaskBase::TaskId() const {
    return task_id_;
}

int BaselineTaskBase::QueryTaskSnapshotJson(std::string* out_json) const {
    if (!out_json) return error::BAD_REQUEST;

    std::lock_guard<std::mutex> lock(mutex_);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("task_id");
    writer.String(task_id_.c_str());
    writer.Key("name");
    writer.String(task_name_.c_str());
    writer.Key("kind");
    writer.String(KindName(kind_));
    writer.Key("closed");
    writer.Bool(closed_);
    writer.EndObject();

    *out_json = buf.GetString();
    return error::OK;
}

int BaselineTaskBase::QuerySeriesSnapshotJson(const BaselineStringRef& key,
                                              std::string* out_json) const {
    if (!out_json) return error::BAD_REQUEST;

    std::lock_guard<std::mutex> lock(mutex_);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("task_id");
    writer.String(task_id_.c_str());
    writer.Key("key");
    const std::string key_copy = CopyStringRef(key);
    writer.String(key_copy.c_str());
    writer.Key("status");
    writer.String("not_ready");
    writer.EndObject();

    *out_json = buf.GetString();
    return error::OK;
}

int BaselineTaskBase::RequestRebuild(const BaselineStringRef&,
                                     BaselineRebuildReason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (EnsureOpenLocked() != error::OK) return error::UNAVAILABLE;
    return error::UNAVAILABLE;
}

int BaselineTaskBase::Close() {
    // task 由 registry 持有 shared_ptr，而对外只暴露借用指针。
    // Close() 先拿一份自引用，确保从 registry 移除后当前调用栈仍然安全。
    std::shared_ptr<BaselineTaskBase> self = shared_from_this();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return error::OK;
        closed_ = true;
        OnClosingLocked();
    }

    if (registry_) registry_->Unregister(task_id_, this);
    return error::OK;
}

int BaselineTaskBase::EnsureOpenLocked() const {
    return closed_ ? error::UNAVAILABLE : error::OK;
}

std::string BaselineTaskBase::CopyStringRef(const BaselineStringRef& ref) {
    if (!ref.data || ref.size == 0) return "";
    return std::string(ref.data, ref.size);
}

void BaselineTaskBase::OnClosingLocked() {}

const char* BaselineTaskBase::KindName(BaselineTaskKind kind) {
    switch (kind) {
        case BaselineTaskKind::kValue:
            return "value";
        case BaselineTaskKind::kRatio:
            return "ratio";
        case BaselineTaskKind::kRelation:
            return "relation";
    }
    return "unknown";
}

BaselineRelationTask::BaselineRelationTask(TaskRegistry* registry,
                                           RebuildQueue* rebuild_queue,
                                           std::string task_id,
                                           const RelationTaskSpec& spec)
    : BaselineTaskBase(registry,
                       rebuild_queue,
                       std::move(task_id),
                       BaselineTaskKind::kRelation,
                       spec.name,
                       spec.config_json),
      spec_(spec),
      history_binding_(std::make_shared<RelationHistoryBinding>()) {
    history_binding_->reader = nullptr;
    RelationRouter::BuildRoutedFeatureSpecs(spec_, &routed_feature_specs_);
    EnsureRoutedDetectors();
    rebuild_runtime_ = std::make_shared<RebuildTaskRuntime>(
        TaskId(),
        [this](const RebuildRequest& request) { return ExecuteRebuild(request); });
}

void BaselineRelationTask::EnsureRoutedDetectors() {
    for (const auto& routed_feature : routed_feature_specs_) {
        if (routed_feature.detector_kind == RelationRoutedDetectorKind::kValue) {
            if (value_cores_by_routed_feature_.find(routed_feature.routed_feature_id) !=
                value_cores_by_routed_feature_.end()) {
                continue;
            }

            ValueDetectorCoreSpec core_spec;
            core_spec.owner_task_id = TaskId();
            core_spec.routed_feature_id = routed_feature.routed_feature_id;
            core_spec.feature_type = routed_feature.feature_type;
            core_spec.feature_profile = routed_feature.feature_profile;
            value_cores_by_routed_feature_.emplace(
                routed_feature.routed_feature_id,
                std::make_shared<ValueDetectorCore>(core_spec));
            continue;
        }

        if (ratio_cores_by_routed_feature_.find(routed_feature.routed_feature_id) !=
            ratio_cores_by_routed_feature_.end()) {
            continue;
        }

        RatioDetectorCoreSpec core_spec;
        core_spec.owner_task_id = TaskId();
        core_spec.routed_feature_id = routed_feature.routed_feature_id;
        core_spec.feature_type = routed_feature.feature_type;
        core_spec.feature_profile = routed_feature.feature_profile;
        ratio_cores_by_routed_feature_.emplace(
            routed_feature.routed_feature_id,
            std::make_shared<RatioDetectorCore>(core_spec));
    }
}

const char* BaselineRelationTask::Id() const { return BaselineTaskBase::Id(); }
const char* BaselineRelationTask::Name() const { return BaselineTaskBase::Name(); }
BaselineTaskKind BaselineRelationTask::Kind() const { return BaselineTaskBase::Kind(); }
const char* BaselineRelationTask::ConfigJson() const { return BaselineTaskBase::ConfigJson(); }
int BaselineRelationTask::QueryTaskSnapshotJson(std::string* out_json) const {
    if (!out_json) return error::BAD_REQUEST;

    RebuildTaskRuntimeSnapshot rebuild_snapshot;
    if (rebuild_runtime_) rebuild_runtime_->Snapshot(&rebuild_snapshot);
    size_t key_runtime_count = 0;
    {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        key_runtime_count = runtime_by_key_.size();
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("task_id");
    writer.String(Id());
    writer.Key("name");
    writer.String(Name());
    writer.Key("kind");
    writer.String("relation");
    writer.Key("feature_base");
    writer.String(spec_.feature_base.c_str());
    writer.Key("group_space_id");
    writer.String(spec_.group_space_id.c_str());
    writer.Key("metric_count");
    writer.Uint64(spec_.metrics.size());
    writer.Key("routed_feature_count");
    writer.Uint64(routed_feature_specs_.size());
    writer.Key("key_runtime_count");
    writer.Uint64(key_runtime_count);
    writer.Key("reader_bound");
    writer.Bool(history_binding_ && history_binding_->HasReader());
    writer.Key("rebuild_pending");
    writer.Uint64(rebuild_snapshot.pending_count);
    writer.Key("rebuild_inflight");
    writer.Uint64(rebuild_snapshot.inflight_count);
    writer.Key("rebuild_completed");
    writer.Uint64(rebuild_snapshot.completed_count);
    writer.Key("last_rebuild_status");
    writer.Int(rebuild_snapshot.last_status);
    writer.Key("last_rebuild_reason");
    writer.String(rebuild_snapshot.last_reason.c_str());
    writer.Key("last_rebuild_key");
    writer.String(rebuild_snapshot.last_key.c_str());
    writer.EndObject();
    *out_json = buf.GetString();
    return error::OK;
}
int BaselineRelationTask::QuerySeriesSnapshotJson(const BaselineStringRef& key,
                                                  std::string* out_json) const {
    if (!out_json) return error::BAD_REQUEST;

    RelationTaskKeyRuntimeState runtime_state;
    {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        auto it = runtime_by_key_.find(CopyKey(key));
        if (it != runtime_by_key_.end()) runtime_state = it->second;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("task_id");
    writer.String(Id());
    writer.Key("key");
    const std::string key_copy = CopyKey(key);
    writer.String(key_copy.c_str());
    writer.Key("seen_block_count");
    writer.Uint64(runtime_state.seen_block_count);
    writer.Key("basis_metric_count");
    writer.Uint64(runtime_state.service_basis_by_metric.size());
    writer.Key("candidate_state");
    writer.String(runtime_state.candidate_state.c_str());
    writer.Key("switch_state");
    writer.String(runtime_state.switch_state.c_str());
    writer.Key("lineage_compatibility");
    writer.String(RelationLineageCompatibilityName(
        runtime_state.last_lineage_compatibility));
    writer.Key("last_candidate_loss");
    writer.Double(runtime_state.last_candidate_loss);
    writer.Key("last_incumbent_loss");
    writer.Double(runtime_state.last_incumbent_loss);
    writer.Key("validation_feature_count");
    writer.Uint64(runtime_state.validation_feature_count);
    writer.Key("last_replay_observation_count");
    writer.Uint64(runtime_state.last_replay_window.observation_count);
    writer.Key("last_train_observation_count");
    writer.Uint64(runtime_state.last_train_window.observation_count);
    writer.Key("last_holdout_observation_count");
    writer.Uint64(runtime_state.last_holdout_window.observation_count);
    writer.Key("metrics");
    writer.StartArray();
    for (const auto& metric_name : spec_.metrics) {
        writer.StartObject();
        writer.Key("metric_name");
        writer.String(metric_name.c_str());
        auto basis_it = runtime_state.service_basis_by_metric.find(metric_name);
        const bool has_basis = basis_it != runtime_state.service_basis_by_metric.end();
        writer.Key("has_basis");
        writer.Bool(has_basis);
        writer.Key("basis_version");
        writer.Uint64(has_basis ? basis_it->second.basis_version : 0);
        writer.Key("support_size");
        writer.Uint64(has_basis ? basis_it->second.support_explicit.size() : 0);
        writer.Key("stable_head_size");
        writer.Uint64(has_basis ? basis_it->second.stable_head.size() : 0);
        writer.EndObject();
    }
    writer.EndArray();
    writer.EndObject();
    *out_json = buf.GetString();
    return error::OK;
}

int BaselineRelationTask::ExecuteRebuild(const RebuildRequest& request) {
    IBaselineRelationHistoryReader* reader = nullptr;
    {
        std::lock_guard<std::mutex> lock(history_binding_->mutex);
        reader = history_binding_->reader;
    }

    RelationTaskRebuildResult rebuild_result;
    rebuild_result.candidate_state = "fetch_failed";
    rebuild_result.replay_window.request_bucket_start = request.bucket_start_hint;
    rebuild_result.replay_window.request_bucket_end = request.bucket_end;
    rebuild_result.train_window.request_bucket_start = request.bucket_start_hint;
    rebuild_result.train_window.request_bucket_end = request.bucket_end;
    rebuild_result.holdout_window.request_bucket_start = request.bucket_start_hint;
    rebuild_result.holdout_window.request_bucket_end = request.bucket_end;

    auto persist_result = [this, &request](const RelationTaskRebuildResult& result,
                                           bool apply_basis_refresh) {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        auto& runtime_state = runtime_by_key_[request.key];
        runtime_state.candidate_state = result.candidate_state;
        runtime_state.switch_state = result.switch_state;
        runtime_state.last_lineage_compatibility = result.lineage_compatibility;
        runtime_state.last_candidate_loss = result.candidate_loss;
        runtime_state.last_incumbent_loss = result.incumbent_loss;
        runtime_state.validation_feature_count = result.validation_feature_count;
        runtime_state.last_replay_window = result.replay_window;
        runtime_state.last_train_window = result.train_window;
        runtime_state.last_holdout_window = result.holdout_window;
        if (apply_basis_refresh) {
            runtime_state.service_basis_by_metric = result.service_basis_by_metric;
        }
    };

    if (!reader) {
        persist_result(rebuild_result, false);
        return error::UNAVAILABLE;
    }

    const BaselineStringRef key_ref{
        request.key.c_str(),
        static_cast<uint32_t>(request.key.size())};
    const HistoryFetchRequest fetch_req{
        key_ref,
        request.bucket_start_hint,
        request.bucket_end};

    std::vector<OwnedRelationObservationBlock> blocks;
    int64_t last_bucket_id = 0;
    const int fetch_rc = reader->Fetch(fetch_req, [&](const RelationObservationBlock& block) {
        OwnedRelationObservationBlock owned_block;
        const int copy_rc = CopyRelationBlock(spec_, request.key, block, &owned_block);
        if (copy_rc != error::OK) return copy_rc;
        if (!blocks.empty() && owned_block.bucket_id < last_bucket_id) {
            return error::BAD_REQUEST;
        }
        last_bucket_id = owned_block.bucket_id;
        blocks.push_back(std::move(owned_block));
        return error::OK;
    });
    if (fetch_rc != error::OK) {
        rebuild_result.candidate_state =
            fetch_rc == error::BAD_REQUEST ? "replay_failed" : "fetch_failed";
        persist_result(rebuild_result, false);
        return fetch_rc;
    }

    rebuild_result.replay_window = BuildRelationWindowSummary(
        blocks, 0, blocks.size(), request.bucket_start_hint, request.bucket_end);
    const std::size_t holdout_count = DecideRelationHoldoutCount(blocks.size());
    const std::size_t fit_count = blocks.size() - holdout_count;
    rebuild_result.train_window = BuildRelationWindowSummary(
        blocks, 0, fit_count, request.bucket_start_hint, request.bucket_end);
    rebuild_result.holdout_window = BuildRelationWindowSummary(
        blocks, fit_count, blocks.size(), request.bucket_start_hint, request.bucket_end);

    if (blocks.empty()) {
        rebuild_result.candidate_state = "empty";
        persist_result(rebuild_result, false);
        return error::OK;
    }

    RelationTaskKeyRuntimeState incumbent_state;
    {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        auto it = runtime_by_key_.find(request.key);
        if (it != runtime_by_key_.end()) incumbent_state = it->second;
    }

    std::vector<bool> comparable_metric(spec_.metrics.size(), false);
    std::vector<std::vector<RelationMetricSummary>> eval_summaries(spec_.metrics.size());
    std::vector<std::vector<RelationMetricSummary>> service_summaries(spec_.metrics.size());

    // relation rebuild 对每个 metric 同时构建两套 basis 语义：
    // 1. candidate `ServiceBasis`：切换成功后真正对外服务的新摘要语义；
    // 2. `EvalBasis`：若 incumbent lineage 仍兼容，则沿用 incumbent basis，
    //    让 candidate / incumbent 的验证损失落在同一可比空间里。
    for (std::size_t metric_index = 0; metric_index < spec_.metrics.size(); ++metric_index) {
        const std::string& metric_name = spec_.metrics[metric_index];
        const RelationServiceBasis* incumbent_basis = nullptr;
        auto incumbent_it = incumbent_state.service_basis_by_metric.find(metric_name);
        if (incumbent_it != incumbent_state.service_basis_by_metric.end()) {
            incumbent_basis = &incumbent_it->second;
        }

        const uint64_t next_basis_version =
            incumbent_basis ? (incumbent_basis->basis_version + 1) : 1;
        RelationServiceBasis candidate_basis;
        const auto basis_input = BuildRelationBasisInput(
            spec_, blocks, static_cast<uint32_t>(metric_index), fit_count, next_basis_version);
        if (RelationBasisBuilder::BuildServiceBasis(basis_input, &candidate_basis) != error::OK) {
            rebuild_result.candidate_state = "basis_build_failed";
            persist_result(rebuild_result, false);
            return error::OK;
        }

        rebuild_result.service_basis_by_metric.emplace(metric_name, candidate_basis);

        RelationEvalBasis eval_basis;
        RelationBasisBuilder::BuildEvalBasis(incumbent_basis, spec_, &eval_basis);
        const auto compatibility = eval_basis.compatibility;
        rebuild_result.lineage_compatibility = MergeLineageCompatibility(
            rebuild_result.lineage_compatibility, compatibility);

        if (BuildRelationMetricSummaries(
                request.key,
                blocks,
                static_cast<uint32_t>(metric_index),
                candidate_basis,
                &service_summaries[metric_index]) != error::OK) {
            rebuild_result.candidate_state = "replay_failed";
            persist_result(rebuild_result, false);
            return error::BAD_REQUEST;
        }

        if (eval_basis.has_incumbent &&
            compatibility != RelationLineageCompatibility::kNewLineage) {
            comparable_metric[metric_index] = true;
            if (BuildRelationMetricSummaries(
                    request.key,
                    blocks,
                    static_cast<uint32_t>(metric_index),
                    eval_basis.basis,
                    &eval_summaries[metric_index]) != error::OK) {
                rebuild_result.candidate_state = "replay_failed";
                persist_result(rebuild_result, false);
                return error::BAD_REQUEST;
            }
        }
    }

    double candidate_loss_sum = 0.0;
    double incumbent_loss_sum = 0.0;
    uint64_t comparable_feature_count = 0;
    uint64_t serviceable_feature_count = 0;

    for (const auto& routed_feature : routed_feature_specs_) {
        auto metric_it =
            std::find(spec_.metrics.begin(), spec_.metrics.end(), routed_feature.metric_name);
        if (metric_it == spec_.metrics.end()) continue;
        const std::size_t metric_index =
            static_cast<std::size_t>(std::distance(spec_.metrics.begin(), metric_it));

        RelationApplyPlan plan;
        plan.feature_spec = &routed_feature;

        if (routed_feature.detector_kind == RelationRoutedDetectorKind::kValue) {
            auto core_it =
                value_cores_by_routed_feature_.find(routed_feature.routed_feature_id);
            if (core_it == value_cores_by_routed_feature_.end()) {
                rebuild_result.candidate_state = "core_unavailable";
                persist_result(rebuild_result, false);
                return error::UNAVAILABLE;
            }

            ValueRebuildContext rebuild_context;
            core_it->second->BuildRebuildContext(request.key, &rebuild_context);
            plan.model_version = rebuild_context.next_model_version;

            if (comparable_metric[metric_index]) {
                // 只有在 incumbent basis 仍可比较时，才在共同 EvalBasis 上训练 candidate
                // 并与 incumbent 计损。否则跳过比较，后面按 direct_apply / 冷启动语义处理。
                ValueReplaySeries eval_replay;
                if (BuildRelationValueReplay(
                        routed_feature,
                        request.key,
                        blocks,
                        eval_summaries[metric_index],
                        request.bucket_start_hint,
                        request.bucket_end,
                        &eval_replay) != error::OK) {
                    rebuild_result.candidate_state = "replay_failed";
                    persist_result(rebuild_result, false);
                    return error::BAD_REQUEST;
                }

                ValueCandidateBuildResult build_result;
                CandidateBuilder::BuildValue(
                    core_it->second->profile(),
                    eval_replay,
                    rebuild_context.next_model_version,
                    nullptr,
                    &build_result);
                if (build_result.status != CandidateBuildStatus::kTrained ||
                    !build_result.candidate_model) {
                    rebuild_result.candidate_state =
                        CandidateBuildStatusName(build_result.status);
                    persist_result(rebuild_result, false);
                    return error::OK;
                }

                const auto validation = CandidateValidator::ValidateValue(
                    core_it->second->profile(),
                    eval_replay,
                    build_result.holdout_window,
                    build_result.candidate_model.get(),
                    rebuild_context.incumbent_shadow_state.active
                        ? nullptr
                        : rebuild_context.incumbent_formal_model.get(),
                    rebuild_context.incumbent_shadow_state.active
                        ? &rebuild_context.incumbent_shadow_state
                        : nullptr);
                if (validation.status != CandidateValidationStatus::kPassed &&
                    validation.status != CandidateValidationStatus::kFailed &&
                    validation.status != CandidateValidationStatus::kBypassNoIncumbent) {
                    rebuild_result.candidate_state =
                        CandidateValidationStatusName(validation.status);
                    persist_result(rebuild_result, false);
                    return error::OK;
                }
                if (validation.status != CandidateValidationStatus::kBypassNoIncumbent) {
                    plan.candidate_loss = validation.candidate_loss;
                    plan.incumbent_loss = validation.incumbent_loss;
                    plan.validation_count = validation.validation_count;
                    candidate_loss_sum += validation.candidate_loss;
                    incumbent_loss_sum += validation.incumbent_loss;
                    ++comparable_feature_count;
                }
            }

            // 无论是否存在可比 incumbent，最终对外服务的新 formal 都必须按
            // candidate `ServiceBasis` 重新提取摘要并训练；因为切换后线上看到的
            // 正是这套新的 relation 摘要语义。
            ValueReplaySeries service_replay;
            if (BuildRelationValueReplay(
                    routed_feature,
                    request.key,
                    blocks,
                    service_summaries[metric_index],
                    request.bucket_start_hint,
                    request.bucket_end,
                    &service_replay) != error::OK) {
                rebuild_result.candidate_state = "replay_failed";
                persist_result(rebuild_result, false);
                return error::BAD_REQUEST;
            }
            plan.replay_window = service_replay.window;
            const std::size_t service_holdout = DecideRelationHoldoutCount(
                service_replay.points.size());
            const std::size_t service_train_count =
                service_replay.points.size() - service_holdout;
            plan.train_window =
                BuildReplayWindowSummary(service_replay, 0, service_train_count);
            plan.holdout_window = BuildReplayWindowSummary(
                service_replay, service_train_count, service_replay.points.size());
            if (!service_replay.points.empty()) {
                plan.value_full_model = TrainFullValueModel(
                    core_it->second->profile(),
                    service_replay,
                    rebuild_context.next_model_version);
                if (!plan.value_full_model) {
                    rebuild_result.candidate_state = "train_failed";
                    persist_result(rebuild_result, false);
                    return error::OK;
                }
                ++serviceable_feature_count;
            }
        } else {
            auto core_it =
                ratio_cores_by_routed_feature_.find(routed_feature.routed_feature_id);
            if (core_it == ratio_cores_by_routed_feature_.end()) {
                rebuild_result.candidate_state = "core_unavailable";
                persist_result(rebuild_result, false);
                return error::UNAVAILABLE;
            }

            RatioRebuildContext rebuild_context;
            core_it->second->BuildRebuildContext(request.key, &rebuild_context);
            plan.model_version = rebuild_context.next_model_version;

            if (comparable_metric[metric_index]) {
                // Ratio routed feature 与 value routed feature 完全共用同一套比较原则：
                // 验证永远发生在 EvalBasis，不直接比较不同 basis 下的摘要损失。
                RatioReplaySeries eval_replay;
                if (BuildRelationRatioReplay(
                        routed_feature,
                        request.key,
                        blocks,
                        eval_summaries[metric_index],
                        request.bucket_start_hint,
                        request.bucket_end,
                        &eval_replay) != error::OK) {
                    rebuild_result.candidate_state = "replay_failed";
                    persist_result(rebuild_result, false);
                    return error::BAD_REQUEST;
                }

                RatioCandidateBuildResult build_result;
                CandidateBuilder::BuildRatio(
                    core_it->second->profile(),
                    eval_replay,
                    rebuild_context.next_model_version,
                    nullptr,
                    &build_result);
                if (build_result.status != CandidateBuildStatus::kTrained ||
                    !build_result.candidate_model) {
                    rebuild_result.candidate_state =
                        CandidateBuildStatusName(build_result.status);
                    persist_result(rebuild_result, false);
                    return error::OK;
                }

                const auto validation = CandidateValidator::ValidateRatio(
                    core_it->second->profile(),
                    eval_replay,
                    build_result.holdout_window,
                    build_result.candidate_model.get(),
                    rebuild_context.incumbent_shadow_state.active
                        ? nullptr
                        : rebuild_context.incumbent_formal_model.get(),
                    rebuild_context.incumbent_shadow_state.active
                        ? &rebuild_context.incumbent_shadow_state
                        : nullptr);
                if (validation.status != CandidateValidationStatus::kPassed &&
                    validation.status != CandidateValidationStatus::kFailed &&
                    validation.status != CandidateValidationStatus::kBypassNoIncumbent) {
                    rebuild_result.candidate_state =
                        CandidateValidationStatusName(validation.status);
                    persist_result(rebuild_result, false);
                    return error::OK;
                }
                if (validation.status != CandidateValidationStatus::kBypassNoIncumbent) {
                    plan.candidate_loss = validation.candidate_loss;
                    plan.incumbent_loss = validation.incumbent_loss;
                    plan.validation_count = validation.validation_count;
                    candidate_loss_sum += validation.candidate_loss;
                    incumbent_loss_sum += validation.incumbent_loss;
                    ++comparable_feature_count;
                }
            }

            // ServiceBasis 上训练出的 full model 才是正式切换后继续服务的模型。
            RatioReplaySeries service_replay;
            if (BuildRelationRatioReplay(
                    routed_feature,
                    request.key,
                    blocks,
                    service_summaries[metric_index],
                    request.bucket_start_hint,
                    request.bucket_end,
                    &service_replay) != error::OK) {
                rebuild_result.candidate_state = "replay_failed";
                persist_result(rebuild_result, false);
                return error::BAD_REQUEST;
            }
            plan.replay_window = service_replay.window;
            const std::size_t service_holdout = DecideRelationHoldoutCount(
                service_replay.points.size());
            const std::size_t service_train_count =
                service_replay.points.size() - service_holdout;
            plan.train_window =
                BuildReplayWindowSummary(service_replay, 0, service_train_count);
            plan.holdout_window = BuildReplayWindowSummary(
                service_replay, service_train_count, service_replay.points.size());
            if (!service_replay.points.empty()) {
                plan.ratio_full_model = TrainFullRatioModel(
                    core_it->second->profile(),
                    service_replay,
                    rebuild_context.next_model_version);
                if (!plan.ratio_full_model) {
                    rebuild_result.candidate_state = "train_failed";
                    persist_result(rebuild_result, false);
                    return error::OK;
                }
                ++serviceable_feature_count;
            }
        }

        rebuild_result.apply_plans.push_back(std::move(plan));
    }

    rebuild_result.validation_feature_count = comparable_feature_count;
    if (comparable_feature_count > 0) {
        // relation 任务级验证损失按“所有可比 routed feature 的平均损失”聚合。
        // 只要存在可比 incumbent，就必须先通过这道验证门，再允许 formal_apply。
        rebuild_result.candidate_loss =
            candidate_loss_sum / static_cast<double>(comparable_feature_count);
        rebuild_result.incumbent_loss =
            incumbent_loss_sum / static_cast<double>(comparable_feature_count);
        if (rebuild_result.candidate_loss > 1.05 * rebuild_result.incumbent_loss) {
            rebuild_result.candidate_state = "validation_failed";
            persist_result(rebuild_result, false);
            return error::OK;
        }
        rebuild_result.switch_state = "formal_apply";
    } else {
        // 没有任何可比特征时，不做 incumbent 比较，按 direct_apply 处理。
        // 典型场景是首次建模或判定为 new lineage。
        rebuild_result.switch_state = "direct_apply";
    }

    if (serviceable_feature_count == 0) {
        rebuild_result.candidate_state = "no_serviceable_feature";
        rebuild_result.switch_state = "none";
        persist_result(rebuild_result, false);
        return error::OK;
    }

    // 关系任务只在正式切换成功后刷新 service basis。
    // routed detector core 统一用 replace_formal_model 覆盖或清空旧 formal model，
    // 避免 basis 变化后继续沿用旧摘要语义下训练出的模型。
    for (const auto& plan : rebuild_result.apply_plans) {
        if (!plan.feature_spec) continue;

        if (plan.feature_spec->detector_kind == RelationRoutedDetectorKind::kValue) {
            auto core_it =
                value_cores_by_routed_feature_.find(plan.feature_spec->routed_feature_id);
            if (core_it == value_cores_by_routed_feature_.end()) continue;

            ValueApplyFormalModelResult apply_result;
            apply_result.replay_window = plan.replay_window;
            apply_result.train_window = plan.train_window;
            apply_result.holdout_window = plan.holdout_window;
            apply_result.candidate_loss = plan.candidate_loss;
            apply_result.incumbent_loss = plan.incumbent_loss;
            apply_result.validation_count = plan.validation_count;
            apply_result.candidate_trained = true;
            apply_result.candidate_generation = plan.model_version;
            apply_result.switch_state = rebuild_result.switch_state;
            apply_result.replace_formal_model = true;
            apply_result.full_model = plan.value_full_model;
            core_it->second->ApplyFormalModel(request.key, apply_result);
            continue;
        }

        auto core_it =
            ratio_cores_by_routed_feature_.find(plan.feature_spec->routed_feature_id);
        if (core_it == ratio_cores_by_routed_feature_.end()) continue;

        RatioApplyFormalModelResult apply_result;
        apply_result.replay_window = plan.replay_window;
        apply_result.train_window = plan.train_window;
        apply_result.holdout_window = plan.holdout_window;
        apply_result.candidate_loss = plan.candidate_loss;
        apply_result.incumbent_loss = plan.incumbent_loss;
        apply_result.validation_count = plan.validation_count;
        apply_result.candidate_trained = true;
        apply_result.candidate_generation = plan.model_version;
        apply_result.switch_state = rebuild_result.switch_state;
        apply_result.replace_formal_model = true;
        apply_result.full_model = plan.ratio_full_model;
        core_it->second->ApplyFormalModel(request.key, apply_result);
    }

    rebuild_result.candidate_state = "none";
    persist_result(rebuild_result, true);
    return error::OK;
}

int BaselineRelationTask::RequestRebuild(const BaselineStringRef& key,
                                         BaselineRebuildReason reason) {
    if (!key.data || key.size == 0) return error::BAD_REQUEST;

    // 关系任务沿用同一套异步入队约束：在 task 锁内完成 pending 记账与入队，
    // 防止 `Close()` 在两者之间插入造成慢路径计数失衡。
    std::lock_guard<std::mutex> lock(mutex_);
    if (EnsureOpenLocked() != error::OK) return error::UNAVAILABLE;
    if (!rebuild_queue_ || !rebuild_runtime_) return error::UNAVAILABLE;
    if (!rebuild_runtime_->PrepareEnqueue()) return error::UNAVAILABLE;

    RebuildRequest request;
    request.task_kind = BaselineTaskKind::kRelation;
    request.task_id = TaskId();
    request.feature_name = spec_.feature_base;
    request.key = CopyStringRef(key);
    request.rebuild_reason = reason;
    request.bucket_start_hint = 0;
    request.bucket_end = 0;
    request.runtime = rebuild_runtime_;

    const int rc = rebuild_queue_->Push(request);
    if (rc != error::OK) rebuild_runtime_->OnCanceled(request);
    return rc;
}
int BaselineRelationTask::Close() { return BaselineTaskBase::Close(); }

int BaselineRelationTask::SetHistoryReader(IBaselineRelationHistoryReader* reader) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (EnsureOpenLocked() != error::OK) return error::UNAVAILABLE;
    if (rebuild_runtime_ && !rebuild_runtime_->CanSwapReader()) return error::CONFLICT;
    if (history_binding_) {
        std::lock_guard<std::mutex> binding_lock(history_binding_->mutex);
        history_binding_->reader = reader;
    }
    return error::OK;
}

int BaselineRelationTask::SubmitBlock(const RelationObservationBlock& block,
                                      DetectorResult* out) {
    if (!out) return error::BAD_REQUEST;
    *out = DetectorResult{};

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (EnsureOpenLocked() != error::OK) {
            FillUnavailableResult(out);
            return error::UNAVAILABLE;
        }
    }

    const int validate_rc = ValidateRelationBlock(spec_, block);
    if (validate_rc != error::OK) {
        FillBadRequestResult(out);
        return validate_rc;
    }

    const std::string key = CopyKey(block.key);
    std::vector<RelationServiceBasis> bases(spec_.metrics.size());
    {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        auto& runtime_state = runtime_by_key_[key];
        ++runtime_state.seen_block_count;

        for (uint32_t metric_index = 0; metric_index < block.metric_count; ++metric_index) {
            const std::string& metric_name = spec_.metrics[metric_index];
            auto basis_it = runtime_state.service_basis_by_metric.find(metric_name);
            if (basis_it == runtime_state.service_basis_by_metric.end()) {
                RelationServiceBasis bootstrap_basis;
                const auto build_input = MakeBootstrapBasisInput(spec_, block, metric_index);
                if (RelationBasisBuilder::BuildServiceBasis(build_input, &bootstrap_basis) ==
                    error::OK) {
                    basis_it = runtime_state.service_basis_by_metric
                                   .emplace(metric_name, std::move(bootstrap_basis))
                                   .first;
                }
            }

            if (basis_it != runtime_state.service_basis_by_metric.end()) {
                bases[metric_index] = basis_it->second;
            } else {
                bases[metric_index].feature_base = spec_.feature_base;
                bases[metric_index].metric_name = metric_name;
                bases[metric_index].group_space_id = spec_.group_space_id;
                bases[metric_index].group_space_version = spec_.group_space_version;
                bases[metric_index].k_head = spec_.summary_policy.k_head;
            }
        }
    }

    std::vector<RelationMetricSummary> summaries(spec_.metrics.size());
    for (uint32_t metric_index = 0; metric_index < block.metric_count; ++metric_index) {
        const int rc = RelationSummaryExtractor::ExtractMetricSummary(
            block, metric_index, bases[metric_index], &summaries[metric_index]);
        if (rc != error::OK) {
            FillBadRequestResult(out);
            return rc;
        }
    }

    DetectorResult merged_result;
    merged_result.status = error::OK;
    bool has_serviceable = false;
    uint64_t aggregate_flags = 0;
    bool rebuild_required = false;
    BaselineRebuildReason rebuild_reason = BaselineRebuildReason::kShiftConfirmed;
    int64_t rebuild_start_hint = 0;
    int64_t rebuild_end = block.bucket_id;

    for (const auto& routed_feature : routed_feature_specs_) {
        auto metric_it =
            std::find(spec_.metrics.begin(), spec_.metrics.end(), routed_feature.metric_name);
        if (metric_it == spec_.metrics.end()) continue;

        const size_t metric_index =
            static_cast<size_t>(std::distance(spec_.metrics.begin(), metric_it));
        const RelationMetricSummary& summary = summaries[metric_index];

        DetectorSubmitOutput submit_output;
        if (routed_feature.detector_kind == RelationRoutedDetectorKind::kValue) {
            ValueObservation observation;
            if (!RelationRouter::BuildValueObservation(
                    routed_feature, block.key, block.bucket_id, summary, &observation)) {
                continue;
            }
            auto core_it =
                value_cores_by_routed_feature_.find(routed_feature.routed_feature_id);
            if (core_it == value_cores_by_routed_feature_.end()) continue;
            if (core_it->second->Submit(observation, &submit_output) != error::OK) continue;
        } else {
            RatioObservation observation;
            if (!RelationRouter::BuildRatioObservation(
                    routed_feature, block.key, block.bucket_id, summary, &observation)) {
                continue;
            }
            auto core_it =
                ratio_cores_by_routed_feature_.find(routed_feature.routed_feature_id);
            if (core_it == ratio_cores_by_routed_feature_.end()) continue;
            if (core_it->second->Submit(observation, &submit_output) != error::OK) continue;
        }

        aggregate_flags |= submit_output.detector_result.flags;
        MergeFeatureResult(submit_output.detector_result, &has_serviceable, &merged_result);
        MergeRebuildIntent(
            submit_output, &rebuild_required, &rebuild_reason, &rebuild_start_hint, &rebuild_end);
    }

    if (has_serviceable) {
        merged_result.flags |= aggregate_flags;
        *out = merged_result;
    } else {
        out->status = error::OK;
        out->flags = aggregate_flags;
    }

    if (rebuild_required) {
        RebuildRequest request;
        request.task_kind = BaselineTaskKind::kRelation;
        request.task_id = TaskId();
        request.feature_name = spec_.feature_base;
        request.key = key;
        request.rebuild_reason = rebuild_reason;
        request.bucket_start_hint = rebuild_start_hint;
        request.bucket_end = rebuild_end;
        request.runtime = rebuild_runtime_;

        std::lock_guard<std::mutex> lock(mutex_);
        if (EnsureOpenLocked() == error::OK && rebuild_queue_ && rebuild_runtime_ &&
            rebuild_runtime_->PrepareEnqueue()) {
            const int push_rc = rebuild_queue_->Push(request);
            if (push_rc == error::OK) {
                out->flags |= kBaselineFlagRebuildQueued;
            } else {
                rebuild_runtime_->OnCanceled(request);
            }
        }
    }

    return error::OK;
}

void BaselineRelationTask::OnClosingLocked() {
    if (history_binding_) {
        std::lock_guard<std::mutex> binding_lock(history_binding_->mutex);
        history_binding_->reader = nullptr;
    }
    // `Close()` 语义是“返回即失效”，所以这里必须同步排空 / 等待慢路径。
    if (rebuild_queue_) rebuild_queue_->CancelTask(TaskId());
    if (rebuild_runtime_) rebuild_runtime_->CloseAndWait();
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    runtime_by_key_.clear();
    value_cores_by_routed_feature_.clear();
    ratio_cores_by_routed_feature_.clear();
}

}  // namespace baseline
}  // namespace flowsql
