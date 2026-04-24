/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "relation_task.h"

#include <common/error_code.h>

#include <algorithm>
#include <optional>
#include <memory>
#include <unordered_map>
#include <utility>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "plugins/baseline/fusion/key_risk_fusion.h"
#include "plugins/baseline/fusion/relation_pattern_fusion.h"
#include "plugins/baseline/relation/relation_summary_extractor.h"
#include "plugins/baseline/rebuild/candidate_builder.h"
#include "plugins/baseline/rebuild/candidate_validator.h"
#include "plugins/baseline/rebuild/formal_model_trainer.h"
#include "plugins/baseline/rebuild/rebuild_queue.h"
#include "plugins/baseline/rebuild/rebuild_worker.h"

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
        double value_sum = 0.0;
        for (uint32_t j = 0; j < block.nnz; ++j) {
            if (j > 0 && block.group_idx[j - 1] >= block.group_idx[j]) {
                return error::BAD_REQUEST;
            }
            if (metric.values[j] < 0.0) return error::BAD_REQUEST;
            value_sum += metric.values[j];
        }
        if (value_sum > metric.total + 1e-9) return error::BAD_REQUEST;
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
    input.group_space_version = spec.group_space_version.value_or("");
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
    uint32_t flags = 0;
    uint32_t active_count = 0;
    std::vector<double> values;
};

struct OwnedRelationObservationBlock {
    int64_t bucket_id = 0;
    std::vector<uint32_t> group_idx;
    std::vector<OwnedRelationMetricBlock> metrics;
};

struct RelationApplyPlan {
    RelationRoutedFeatureSpec feature_spec;
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
    std::unordered_map<std::string, RelationEvalBasis> eval_basis_by_metric;
    std::vector<RelationApplyPlan> apply_plans;
};

bool SameBaselineSourceConfig(const BaselineSourceConfig& lhs,
                              const BaselineSourceConfig& rhs) {
    if (lhs.sources.size() != rhs.sources.size()) return false;
    for (std::size_t i = 0; i < lhs.sources.size(); ++i) {
        if (lhs.sources[i].source_key != rhs.sources[i].source_key) return false;
    }
    return true;
}

bool SameOptionalBaselineSourceConfig(const std::optional<BaselineSourceConfig>& lhs,
                                      const std::optional<BaselineSourceConfig>& rhs) {
    if (lhs.has_value() != rhs.has_value()) return false;
    if (!lhs.has_value()) return true;
    return SameBaselineSourceConfig(*lhs, *rhs);
}

bool SameEventCalendarEntry(const EventCalendarEntry& lhs,
                            const EventCalendarEntry& rhs) {
    return lhs.event_code == rhs.event_code && lhs.scope_type == rhs.scope_type &&
           lhs.alignment_mode == rhs.alignment_mode && lhs.start_ts == rhs.start_ts &&
           lhs.end_ts == rhs.end_ts && lhs.enabled == rhs.enabled &&
           lhs.feature == rhs.feature && lhs.key == rhs.key && lhs.tz == rhs.tz;
}

bool SameOptionalEventCalendarSpec(const std::optional<EventCalendarSpec>& lhs,
                                   const std::optional<EventCalendarSpec>& rhs) {
    if (lhs.has_value() != rhs.has_value()) return false;
    if (!lhs.has_value()) return true;
    if (lhs->calendar_id != rhs->calendar_id ||
        lhs->calendar_version != rhs->calendar_version ||
        lhs->entries.size() != rhs->entries.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs->entries.size(); ++i) {
        if (!SameEventCalendarEntry(lhs->entries[i], rhs->entries[i])) return false;
    }
    return true;
}

bool CanReuseRoutedFeatureRuntime(const RelationRoutedFeatureRuntime& runtime,
                                  const RelationRoutedFeatureSpec& spec) {
    if (runtime.spec.detector_kind != spec.detector_kind ||
        runtime.spec.feature_type != spec.feature_type ||
        runtime.spec.feature_profile != spec.feature_profile ||
        runtime.spec.transform_kind != spec.transform_kind ||
        runtime.spec.delta != spec.delta ||
        runtime.spec.tz != spec.tz ||
        !SameOptionalBaselineSourceConfig(runtime.spec.baseline_source_config,
                                          spec.baseline_source_config) ||
        !SameOptionalEventCalendarSpec(runtime.spec.event_calendar_spec,
                                       spec.event_calendar_spec)) {
        return false;
    }
    return true;
}

StoredRelationDetectorResult CaptureDetectorResult(const DetectorResult& result) {
    StoredRelationDetectorResult stored;
    stored.available = true;
    stored.status = result.status;
    stored.ts = result.ts;
    stored.raw_score = result.raw_score;
    stored.normalized_score = result.normalized_score;
    stored.confidence = result.confidence;
    stored.persistence = result.persistence;
    stored.direction = result.direction;
    stored.severity = result.severity;
    stored.provider = result.provider;
    stored.reason_code = result.reason_code;
    stored.feature = result.feature.data && result.feature.size > 0
                         ? std::string(result.feature.data, result.feature.size)
                         : "";
    stored.feature_type = result.feature_type.data && result.feature_type.size > 0
                              ? std::string(result.feature_type.data, result.feature_type.size)
                              : "";
    if (result.evidence.kind == BaselineEvidenceKind::kValue) {
        stored.baseline_source_kind = result.evidence.value.baseline_source_kind;
        if ((result.evidence.value.field_flags & kBaselineEvidenceHasSourceKey) != 0 &&
            result.evidence.value.baseline_source_key.data &&
            result.evidence.value.baseline_source_key.size > 0) {
            stored.baseline_source_key = std::string(result.evidence.value.baseline_source_key.data,
                                                     result.evidence.value.baseline_source_key.size);
        }
    } else if (result.evidence.kind == BaselineEvidenceKind::kRatio) {
        stored.baseline_source_kind = result.evidence.ratio.baseline_source_kind;
        if ((result.evidence.ratio.field_flags & kBaselineEvidenceHasSourceKey) != 0 &&
            result.evidence.ratio.baseline_source_key.data &&
            result.evidence.ratio.baseline_source_key.size > 0) {
            stored.baseline_source_key = std::string(result.evidence.ratio.baseline_source_key.data,
                                                     result.evidence.ratio.baseline_source_key.size);
        }
    }
    return stored;
}

FusionResult BuildPatternContributionResult(const std::string& key,
                                            int64_t ts,
                                            const FusionPatternContribution& pattern) {
    FusionResult result;
    result.key = MakeOwnedStringRef(key);
    result.ts = ts;
    result.risk = pattern.projection.weighted_score;
    result.dominant_pattern_count = 1;
    result.dominant_pattern[0].pattern = MakeOwnedStringRef(pattern.projection.pattern);
    result.dominant_pattern[0].feature_base =
        MakeOwnedStringRef(pattern.projection.feature_base);
    result.dominant_pattern[0].score_pattern = pattern.projection.score_pattern;

    const uint32_t metric_count = std::min<uint32_t>(
        static_cast<uint32_t>(pattern.projection.metrics_hit.size()),
        kBaselinePatternMetricsHitLimit);
    result.dominant_pattern[0].metrics_hit_count = metric_count;
    for (uint32_t i = 0; i < metric_count; ++i) {
        result.dominant_pattern[0].metrics_hit[i] =
            MakeOwnedStringRef(pattern.projection.metrics_hit[i]);
    }

    const uint32_t support_count = std::min<uint32_t>(
        static_cast<uint32_t>(pattern.projection.supporting_features.size()),
        kBaselinePatternSupportingFeatureLimit);
    result.dominant_pattern[0].supporting_feature_count = support_count;
    for (uint32_t i = 0; i < support_count; ++i) {
        result.dominant_pattern[0].supporting_features[i] =
            MakeOwnedStringRef(pattern.projection.supporting_features[i]);
    }
    return result;
}

const StoredFusionResult* SelectResultForBucket(const KeyRiskFusionSnapshot& snapshot,
                                                int64_t bucket_id) {
    if (snapshot.active_window.available && snapshot.active_window.ts == bucket_id) {
        return &snapshot.active_window;
    }
    if (snapshot.latest_finalized_result.available &&
        snapshot.latest_finalized_result.ts == bucket_id) {
        return &snapshot.latest_finalized_result;
    }
    return nullptr;
}

RelationRoutedFeatureRuntime BuildRoutedFeatureRuntime(const std::string& task_id,
                                                       const std::string& key,
                                                       const RelationRoutedFeatureSpec& spec) {
    RelationRoutedFeatureRuntime runtime;
    runtime.spec = spec;

    if (spec.detector_kind == RelationRoutedDetectorKind::kValue) {
        ValueDetectorCoreSpec core_spec;
        core_spec.owner_task_id = task_id;
        core_spec.routed_feature_id = spec.feature;
        core_spec.feature_type = spec.feature_type;
        core_spec.feature_profile = spec.feature_profile;
        core_spec.transform_kind = spec.transform_kind;
        core_spec.event_calendar_spec = spec.event_calendar_spec;
        if (spec.baseline_source_config.has_value()) {
            core_spec.baseline_source_configs.push_back(
                SeriesBaselineSourceConfig{key, *spec.baseline_source_config});
        }
        runtime.value_core = std::make_shared<ValueDetectorCore>(core_spec);
        return runtime;
    }

    RatioDetectorCoreSpec core_spec;
    core_spec.owner_task_id = task_id;
    core_spec.routed_feature_id = spec.feature;
    core_spec.feature_type = spec.feature_type;
    core_spec.feature_profile = spec.feature_profile;
    core_spec.event_calendar_spec = spec.event_calendar_spec;
    if (spec.baseline_source_config.has_value()) {
        core_spec.baseline_source_configs.push_back(
            SeriesBaselineSourceConfig{key, *spec.baseline_source_config});
    }
    runtime.ratio_core = std::make_shared<RatioDetectorCore>(core_spec);
    return runtime;
}

void MaterializeMetricRuntime(const std::string& task_id,
                              const std::string& key,
                              const RelationTaskSpec& spec,
                              const RelationTaskClockSpec& clock_spec,
                              const std::optional<EventCalendarSpec>& event_calendar_spec,
                              IBaselineSourceResolver* source_resolver,
                              const std::string& metric_name,
                              RelationMetricRuntimeState* metric_runtime) {
    if (!metric_runtime) return;

    RelationTaskSpec metric_spec = spec;
    metric_spec.metrics = {metric_name};

    std::vector<RelationRoutedFeatureSpec> next_specs;
    RelationRouter::BuildRoutedFeatureSpecs(
        metric_spec,
        metric_runtime->service_basis,
        clock_spec,
        BaselineStringRef{key.c_str(), static_cast<uint32_t>(key.size())},
        event_calendar_spec ? &(*event_calendar_spec) : nullptr,
        source_resolver,
        &next_specs);

    std::unordered_map<std::string, RelationRoutedFeatureRuntime> old_by_feature;
    for (auto& routed_feature : metric_runtime->routed_features) {
        old_by_feature.emplace(routed_feature.spec.feature, std::move(routed_feature));
    }

    std::vector<RelationRoutedFeatureRuntime> refreshed;
    refreshed.reserve(next_specs.size());
    for (const auto& next_spec : next_specs) {
        auto old_it = old_by_feature.find(next_spec.feature);
        if (old_it != old_by_feature.end() &&
            CanReuseRoutedFeatureRuntime(old_it->second, next_spec)) {
            RelationRoutedFeatureRuntime reused = std::move(old_it->second);
            reused.spec = next_spec;
            refreshed.push_back(std::move(reused));
            continue;
        }

        refreshed.push_back(BuildRoutedFeatureRuntime(task_id, key, next_spec));
    }

    metric_runtime->routed_features = std::move(refreshed);
}

RelationRoutedFeatureRuntime* FindRoutedFeatureRuntime(RelationMetricRuntimeState* metric_runtime,
                                                       const std::string& feature) {
    if (!metric_runtime) return nullptr;
    for (auto& routed_feature : metric_runtime->routed_features) {
        if (routed_feature.spec.feature == feature) return &routed_feature;
    }
    return nullptr;
}

const RelationRoutedFeatureRuntime* FindRoutedFeatureRuntime(
    const RelationMetricRuntimeState* metric_runtime,
    const std::string& feature) {
    if (!metric_runtime) return nullptr;
    for (const auto& routed_feature : metric_runtime->routed_features) {
        if (routed_feature.spec.feature == feature) return &routed_feature;
    }
    return nullptr;
}

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

std::shared_ptr<ValueFormalModel> TrainFullValueModel(const ValueFeatureProfile& profile,
                                                      const ValueReplaySeries& replay,
                                                      uint64_t model_version,
                                                      int64_t delta,
                                                      const std::string& tz) {
    ValueFormalTrainResult train_result;
    const ValueFormalTrainInput input{
        &profile,
        &replay,
        replay.points.size(),
        model_version,
        0,
        replay.window,
        nullptr,
        delta,
        tz,
        nullptr,
        nullptr};
    if (FormalModelTrainer::TrainValue(input, &train_result) != FormalTrainFailureCode::kNone) {
        return nullptr;
    }
    return train_result.model;
}

std::shared_ptr<RatioFormalModel> TrainFullRatioModel(const RatioFeatureProfile& profile,
                                                      const RatioReplaySeries& replay,
                                                      uint64_t model_version,
                                                      int64_t delta,
                                                      const std::string& tz) {
    RatioFormalTrainResult train_result;
    const RatioFormalTrainInput input{
        &profile,
        &replay,
        replay.points.size(),
        model_version,
        0,
        replay.window,
        nullptr,
        delta,
        tz,
        nullptr,
        nullptr};
    if (FormalModelTrainer::TrainRatio(input, &train_result) != FormalTrainFailureCode::kNone) {
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
        metric.flags = block.metrics[i].flags;
        metric.active_count = block.metrics[i].active_count;
        if (block.nnz > 0) {
            metric.values.assign(block.metrics[i].values, block.metrics[i].values + block.nnz);
        }
        owned.metrics.push_back(std::move(metric));
    }

    *out_block = std::move(owned);
    return error::OK;
}

RelationObservationBlock BuildRelationBlockView(const std::string& key,
                                                const OwnedRelationObservationBlock& owned_block,
                                                std::vector<RelationMetricBlock>* metric_views) {
    if (metric_views) {
        metric_views->clear();
        metric_views->reserve(owned_block.metrics.size());
        for (const auto& metric : owned_block.metrics) {
            metric_views->push_back(RelationMetricBlock{
                metric.total,
                metric.flags,
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

RelationBasisBuildInput BuildRelationBasisInput(const RelationTaskSpec& spec,
                                                const std::vector<OwnedRelationObservationBlock>& blocks,
                                                uint32_t metric_index,
                                                std::size_t block_limit,
                                                uint64_t basis_version) {
    RelationBasisBuildInput input;
    input.basis_version = basis_version;
    input.feature_base = spec.feature_base;
    input.metric_name = spec.metrics[metric_index];
    input.group_space_id = spec.group_space_id;
    input.group_space_version = spec.group_space_version.value_or("");
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

int BuildRelationMetricSummaries(const std::string& key,
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

int BuildRelationValueReplay(const RelationRoutedFeatureSpec& feature_spec,
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

int BuildRelationRatioReplay(const RelationRoutedFeatureSpec& feature_spec,
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

RelationLineageCompatibility MergeLineageCompatibility(RelationLineageCompatibility lhs,
                                                       RelationLineageCompatibility rhs) {
    return static_cast<int32_t>(lhs) >= static_cast<int32_t>(rhs) ? lhs : rhs;
}

}  // namespace

BaselineRelationTask::BaselineRelationTask(TaskRegistry* registry,
                                           RebuildQueue* rebuild_queue,
                                           std::string task_id,
                                           const RelationTaskSpec& spec,
                                           const RelationTaskClockSpec& clock_spec,
                                           const std::optional<EventCalendarSpec>& event_calendar_spec,
                                           IBaselineSourceResolver* source_resolver,
                                           KeyRiskFusion* key_risk_fusion)
    : BaselineTaskBase(registry,
                       rebuild_queue,
                       std::move(task_id),
                       BaselineTaskKind::kRelation,
                       spec.name,
                       spec.config_json),
      spec_(spec),
      clock_spec_(clock_spec),
      event_calendar_spec_(event_calendar_spec),
      source_resolver_(source_resolver),
      history_binding_(std::make_shared<RelationHistoryBinding>()),
      key_risk_fusion_(key_risk_fusion) {
    history_binding_->reader = nullptr;
    rebuild_runtime_ = std::make_shared<RebuildTaskRuntime>(
        TaskId(),
        [this](const RebuildRequest& request) { return ExecuteRebuild(request); });
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
    size_t routed_feature_count = 0;
    {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        key_runtime_count = runtime_by_key_.size();
        for (const auto& entry : runtime_by_key_) {
            for (const auto& metric_entry : entry.second.metrics_by_name) {
                routed_feature_count += metric_entry.second.routed_features.size();
            }
        }
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
    writer.Key("delta");
    writer.Int64(clock_spec_.delta);
    writer.Key("tz");
    writer.String(clock_spec_.tz.c_str());
    writer.Key("event_calendar_present");
    writer.Bool(event_calendar_spec_.has_value());
    writer.Key("event_calendar_id");
    writer.String(event_calendar_spec_ ? event_calendar_spec_->calendar_id.c_str() : "");
    writer.Key("event_calendar_version");
    writer.String(event_calendar_spec_ ? event_calendar_spec_->calendar_version.c_str() : "");
    writer.Key("event_calendar_entry_count");
    writer.Uint64(event_calendar_spec_ ? event_calendar_spec_->entries.size() : 0);
    writer.Key("source_resolver_bound");
    writer.Bool(source_resolver_ != nullptr);
    writer.Key("metric_count");
    writer.Uint64(spec_.metrics.size());
    writer.Key("routed_feature_count");
    writer.Uint64(routed_feature_count);
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
    writer.Uint64(runtime_state.metrics_by_name.size());
    writer.Key("candidate_state");
    writer.String(runtime_state.candidate_state.c_str());
    writer.Key("switch_state");
    writer.String(runtime_state.switch_state.c_str());
    writer.Key("lineage_compatibility");
    writer.String(RelationLineageCompatibilityName(runtime_state.last_lineage_compatibility));
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
    writer.Key("last_fusion_result");
    writer.StartObject();
    writer.Key("available");
    writer.Bool(runtime_state.last_fusion_result.available);
    writer.Key("ts");
    writer.Int64(runtime_state.last_fusion_result.ts);
    writer.Key("risk");
    writer.Double(runtime_state.last_fusion_result.risk);
    writer.Key("dominant_single_count");
    writer.Uint(static_cast<uint32_t>(runtime_state.last_fusion_result.dominant_singles.size()));
    writer.Key("dominant_single");
    writer.StartArray();
    for (const auto& dominant : runtime_state.last_fusion_result.dominant_singles) {
        writer.StartObject();
        writer.Key("feature");
        writer.String(dominant.feature.c_str());
        writer.Key("a_f");
        writer.Double(dominant.a_f);
        writer.Key("normalized_score");
        writer.Double(dominant.normalized_score);
        writer.Key("confidence");
        writer.Double(dominant.confidence);
        writer.Key("persistence");
        writer.Uint(dominant.persistence);
        writer.EndObject();
    }
    writer.EndArray();
    writer.Key("dominant_pattern_count");
    writer.Uint(static_cast<uint32_t>(runtime_state.last_fusion_result.dominant_patterns.size()));
    writer.Key("dominant_pattern");
    writer.StartArray();
    for (const auto& pattern : runtime_state.last_fusion_result.dominant_patterns) {
        writer.StartObject();
        writer.Key("pattern");
        writer.String(pattern.pattern.c_str());
        writer.Key("feature_base");
        writer.String(pattern.feature_base.c_str());
        writer.Key("score_pattern");
        writer.Double(pattern.score_pattern);
        writer.Key("metrics_hit");
        writer.StartArray();
        for (const auto& metric : pattern.metrics_hit) writer.String(metric.c_str());
        writer.EndArray();
        writer.Key("supporting_features");
        writer.StartArray();
        for (const auto& feature : pattern.supporting_features) {
            writer.String(feature.c_str());
        }
        writer.EndArray();
        writer.EndObject();
    }
    writer.EndArray();
    writer.EndObject();
    writer.Key("metrics");
    writer.StartArray();
    for (const auto& metric_name : spec_.metrics) {
        writer.StartObject();
        writer.Key("metric_name");
        writer.String(metric_name.c_str());
        auto metric_it = runtime_state.metrics_by_name.find(metric_name);
        const bool has_basis = metric_it != runtime_state.metrics_by_name.end();
        const RelationMetricRuntimeState* metric_runtime =
            has_basis ? &metric_it->second : nullptr;
        writer.Key("has_basis");
        writer.Bool(has_basis);
        writer.Key("basis_version");
        writer.Uint64(has_basis ? metric_runtime->service_basis.basis_version : 0);
        writer.Key("support_size");
        writer.Uint64(has_basis ? metric_runtime->service_basis.support_explicit.size() : 0);
        writer.Key("stable_head_size");
        writer.Uint64(has_basis ? metric_runtime->service_basis.stable_head.size() : 0);
        writer.Key("service_basis");
        writer.StartObject();
        writer.Key("basis_version");
        writer.Uint64(has_basis ? metric_runtime->service_basis.basis_version : 0);
        writer.Key("group_space_id");
        writer.String(has_basis ? metric_runtime->service_basis.group_space_id.c_str() : "");
        writer.Key("group_space_version");
        writer.String(has_basis ? metric_runtime->service_basis.group_space_version.c_str() : "");
        writer.Key("support_explicit");
        writer.StartArray();
        if (has_basis) {
            for (uint32_t group_idx : metric_runtime->service_basis.support_explicit) {
                writer.Uint(group_idx);
            }
        }
        writer.EndArray();
        writer.Key("stable_head");
        writer.StartArray();
        if (has_basis) {
            for (uint32_t group_idx : metric_runtime->service_basis.stable_head) {
                writer.Uint(group_idx);
            }
        }
        writer.EndArray();
        writer.EndObject();
        writer.Key("eval_basis");
        writer.StartObject();
        writer.Key("has_incumbent");
        writer.Bool(has_basis && metric_runtime->eval_basis.has_incumbent);
        writer.Key("compatibility");
        writer.String(has_basis
                          ? RelationLineageCompatibilityName(
                                metric_runtime->eval_basis.compatibility)
                          : RelationLineageCompatibilityName(
                                RelationLineageCompatibility::kCompatible));
        writer.EndObject();
        writer.Key("routed_feature_count");
        writer.Uint64(has_basis ? metric_runtime->routed_features.size() : 0);
        writer.Key("routed_features");
        writer.StartArray();
        if (has_basis) {
            for (const auto& routed_feature : metric_runtime->routed_features) {
                writer.StartObject();
                writer.Key("local_slot");
                writer.Int(routed_feature.spec.local_slot);
                writer.Key("feature");
                writer.String(routed_feature.spec.feature.c_str());
                writer.Key("feature_type");
                writer.String(routed_feature.spec.feature_type.c_str());
                writer.Key("feature_profile");
                writer.String(routed_feature.spec.feature_profile.c_str());
                writer.Key("transform_kind");
                if (routed_feature.spec.transform_kind.has_value()) {
                    writer.String(routed_feature.spec.transform_kind->c_str());
                } else {
                    writer.Null();
                }
                writer.Key("delta");
                writer.Int64(routed_feature.spec.delta);
                writer.Key("tz");
                writer.String(routed_feature.spec.tz.c_str());
                writer.Key("baseline_source_present");
                writer.Bool(routed_feature.spec.baseline_source_config.has_value());
                writer.Key("baseline_source_count");
                writer.Uint64(routed_feature.spec.baseline_source_config.has_value()
                                  ? routed_feature.spec.baseline_source_config->sources.size()
                                  : 0);
                writer.Key("event_calendar_present");
                writer.Bool(routed_feature.spec.event_calendar_spec.has_value());
                writer.Key("event_calendar_entry_count");
                writer.Uint64(routed_feature.spec.event_calendar_spec.has_value()
                                  ? routed_feature.spec.event_calendar_spec->entries.size()
                                  : 0);
                writer.Key("last_detector_result");
                writer.StartObject();
                writer.Key("available");
                writer.Bool(routed_feature.last_detector_result.available);
                writer.Key("normalized_score");
                writer.Double(routed_feature.last_detector_result.normalized_score);
                writer.Key("confidence");
                writer.Double(routed_feature.last_detector_result.confidence);
                writer.Key("baseline_source_kind");
                writer.String(BaselineSourceKindName(
                    routed_feature.last_detector_result.baseline_source_kind));
                writer.EndObject();
                writer.EndObject();
            }
        }
        writer.EndArray();
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
            for (const auto& metric_name : spec_.metrics) {
                auto basis_it = result.service_basis_by_metric.find(metric_name);
                if (basis_it == result.service_basis_by_metric.end()) continue;

                auto& metric_runtime = runtime_state.metrics_by_name[metric_name];
                metric_runtime.service_basis = basis_it->second;
                auto eval_it = result.eval_basis_by_metric.find(metric_name);
                if (eval_it != result.eval_basis_by_metric.end()) {
                    metric_runtime.eval_basis = eval_it->second;
                }
                MaterializeMetricRuntime(TaskId(),
                                         request.key,
                                         spec_,
                                         clock_spec_,
                                         event_calendar_spec_,
                                         source_resolver_,
                                         metric_name,
                                         &metric_runtime);
                for (auto& routed_feature : metric_runtime.routed_features) {
                    routed_feature.last_detector_result = StoredRelationDetectorResult{};
                }
            }
        }
    };

    if (!reader) {
        rebuild_result.switch_state = "rebuild_blocked";
        persist_result(rebuild_result, false);
        return error::UNAVAILABLE;
    }

    const BaselineStringRef key_ref{
        request.key.c_str(),
        static_cast<uint32_t>(request.key.size())};
    const HistoryFetchRequest fetch_req{
        key_ref,
        BaselineStringRef{spec_.task_id.c_str(), static_cast<uint32_t>(spec_.task_id.size())},
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

    for (std::size_t metric_index = 0; metric_index < spec_.metrics.size(); ++metric_index) {
        const std::string& metric_name = spec_.metrics[metric_index];
        const RelationServiceBasis* incumbent_basis = nullptr;
        auto incumbent_it = incumbent_state.metrics_by_name.find(metric_name);
        if (incumbent_it != incumbent_state.metrics_by_name.end()) {
            incumbent_basis = &incumbent_it->second.service_basis;
        }

        const uint64_t next_basis_version = incumbent_basis ? (incumbent_basis->basis_version + 1) : 1;
        const auto basis_input = BuildRelationBasisInput(
            spec_, blocks, static_cast<uint32_t>(metric_index), fit_count, next_basis_version);
        RelationMetricCandidateBuildResult basis_build_result;
        if (CandidateBuilder::BuildRelationMetricBases(
                basis_input, incumbent_basis, spec_, &basis_build_result) !=
            CandidateBuildStatus::kTrained) {
            rebuild_result.candidate_state = "basis_build_failed";
            persist_result(rebuild_result, false);
            return error::OK;
        }

        rebuild_result.service_basis_by_metric.emplace(
            metric_name, basis_build_result.candidate_service_basis);
        rebuild_result.eval_basis_by_metric.emplace(metric_name,
                                                    basis_build_result.candidate_eval_basis);
        const auto compatibility = basis_build_result.lineage_compatibility;
        rebuild_result.lineage_compatibility = MergeLineageCompatibility(
            rebuild_result.lineage_compatibility, compatibility);

        if (BuildRelationMetricSummaries(
                request.key,
                blocks,
                static_cast<uint32_t>(metric_index),
                basis_build_result.candidate_service_basis,
                &service_summaries[metric_index]) != error::OK) {
            rebuild_result.candidate_state = "replay_failed";
            persist_result(rebuild_result, false);
            return error::BAD_REQUEST;
        }

        if (basis_build_result.candidate_eval_basis.has_incumbent &&
            compatibility != RelationLineageCompatibility::kNewLineage) {
            comparable_metric[metric_index] = true;
            if (BuildRelationMetricSummaries(
                    request.key,
                    blocks,
                    static_cast<uint32_t>(metric_index),
                    basis_build_result.candidate_eval_basis.basis,
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
    struct ValidationDigest {
        double candidate_loss = 0.0;
        double incumbent_loss = 0.0;
        uint64_t validation_count = 0;
    };

    for (std::size_t metric_index = 0; metric_index < spec_.metrics.size(); ++metric_index) {
        const std::string& metric_name = spec_.metrics[metric_index];
        auto service_basis_it = rebuild_result.service_basis_by_metric.find(metric_name);
        if (service_basis_it == rebuild_result.service_basis_by_metric.end()) continue;

        RelationMetricRuntimeState service_metric_runtime;
        service_metric_runtime.service_basis = service_basis_it->second;
        auto eval_basis_it = rebuild_result.eval_basis_by_metric.find(metric_name);
        if (eval_basis_it != rebuild_result.eval_basis_by_metric.end()) {
            service_metric_runtime.eval_basis = eval_basis_it->second;
        }
        MaterializeMetricRuntime(TaskId(),
                                 request.key,
                                 spec_,
                                 clock_spec_,
                                 event_calendar_spec_,
                                 source_resolver_,
                                 metric_name,
                                 &service_metric_runtime);

        RelationMetricRuntimeState eval_metric_runtime;
        if (comparable_metric[metric_index] &&
            eval_basis_it != rebuild_result.eval_basis_by_metric.end()) {
            eval_metric_runtime.service_basis = eval_basis_it->second.basis;
            eval_metric_runtime.eval_basis = eval_basis_it->second;
            MaterializeMetricRuntime(TaskId(),
                                     request.key,
                                     spec_,
                                     clock_spec_,
                                     event_calendar_spec_,
                                     source_resolver_,
                                     metric_name,
                                     &eval_metric_runtime);
        }

        const RelationMetricRuntimeState* incumbent_metric_runtime = nullptr;
        auto incumbent_metric_it = incumbent_state.metrics_by_name.find(metric_name);
        if (incumbent_metric_it != incumbent_state.metrics_by_name.end()) {
            incumbent_metric_runtime = &incumbent_metric_it->second;
        }

        std::unordered_map<std::string, ValidationDigest> validation_by_feature;
        if (comparable_metric[metric_index]) {
            for (const auto& eval_feature : eval_metric_runtime.routed_features) {
                const auto* incumbent_feature = FindRoutedFeatureRuntime(
                    incumbent_metric_runtime, eval_feature.spec.feature);

                if (eval_feature.spec.detector_kind == RelationRoutedDetectorKind::kValue) {
                    ValueRebuildContext rebuild_context;
                    if (incumbent_feature && incumbent_feature->value_core) {
                        incumbent_feature->value_core->BuildRebuildContext(
                            request.key, &rebuild_context);
                    } else if (eval_feature.value_core) {
                        eval_feature.value_core->BuildRebuildContext(request.key,
                                                                     &rebuild_context);
                    }

                    ValueReplaySeries eval_replay;
                    if (BuildRelationValueReplay(
                            eval_feature.spec,
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

                    const ValueFeatureProfile& profile =
                        incumbent_feature && incumbent_feature->value_core
                            ? incumbent_feature->value_core->profile()
                            : eval_feature.value_core->profile();
                    ValueCandidateBuildResult build_result;
                    CandidateBuilder::BuildValue(profile,
                                                 eval_replay,
                                                 rebuild_context.next_model_version,
                                                 nullptr,
                                                 clock_spec_.delta,
                                                 clock_spec_.tz,
                                                 nullptr,
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
                        profile,
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
                        validation_by_feature[eval_feature.spec.feature] = ValidationDigest{
                            validation.candidate_loss,
                            validation.incumbent_loss,
                            validation.validation_count};
                        candidate_loss_sum += validation.candidate_loss;
                        incumbent_loss_sum += validation.incumbent_loss;
                        ++comparable_feature_count;
                    }
                    continue;
                }

                RatioRebuildContext rebuild_context;
                if (incumbent_feature && incumbent_feature->ratio_core) {
                    incumbent_feature->ratio_core->BuildRebuildContext(request.key,
                                                                       &rebuild_context);
                } else if (eval_feature.ratio_core) {
                    eval_feature.ratio_core->BuildRebuildContext(request.key, &rebuild_context);
                }

                RatioReplaySeries eval_replay;
                if (BuildRelationRatioReplay(
                        eval_feature.spec,
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

                const RatioFeatureProfile& profile =
                    incumbent_feature && incumbent_feature->ratio_core
                        ? incumbent_feature->ratio_core->profile()
                        : eval_feature.ratio_core->profile();
                RatioCandidateBuildResult build_result;
                CandidateBuilder::BuildRatio(profile,
                                             eval_replay,
                                             rebuild_context.next_model_version,
                                             nullptr,
                                             clock_spec_.delta,
                                             clock_spec_.tz,
                                             nullptr,
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
                    profile,
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
                    validation_by_feature[eval_feature.spec.feature] = ValidationDigest{
                        validation.candidate_loss,
                        validation.incumbent_loss,
                        validation.validation_count};
                    candidate_loss_sum += validation.candidate_loss;
                    incumbent_loss_sum += validation.incumbent_loss;
                    ++comparable_feature_count;
                }
            }
        }

        for (const auto& service_feature : service_metric_runtime.routed_features) {
            const auto* incumbent_feature = FindRoutedFeatureRuntime(
                incumbent_metric_runtime, service_feature.spec.feature);
            RelationApplyPlan plan;
            plan.feature_spec = service_feature.spec;

            auto validation_it = validation_by_feature.find(service_feature.spec.feature);
            if (validation_it != validation_by_feature.end()) {
                plan.candidate_loss = validation_it->second.candidate_loss;
                plan.incumbent_loss = validation_it->second.incumbent_loss;
                plan.validation_count = validation_it->second.validation_count;
            }

            if (service_feature.spec.detector_kind == RelationRoutedDetectorKind::kValue) {
                ValueRebuildContext rebuild_context;
                if (incumbent_feature && incumbent_feature->value_core) {
                    incumbent_feature->value_core->BuildRebuildContext(request.key,
                                                                      &rebuild_context);
                } else if (service_feature.value_core) {
                    service_feature.value_core->BuildRebuildContext(request.key,
                                                                    &rebuild_context);
                }
                plan.model_version = rebuild_context.next_model_version;

                ValueReplaySeries service_replay;
                if (BuildRelationValueReplay(
                        service_feature.spec,
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
                const std::size_t service_holdout =
                    DecideRelationHoldoutCount(service_replay.points.size());
                const std::size_t service_train_count =
                    service_replay.points.size() - service_holdout;
                plan.train_window =
                    BuildReplayWindowSummary(service_replay, 0, service_train_count);
                plan.holdout_window = BuildReplayWindowSummary(service_replay,
                                                               service_train_count,
                                                               service_replay.points.size());
                if (!service_replay.points.empty()) {
                    const ValueFeatureProfile& profile =
                        incumbent_feature && incumbent_feature->value_core
                            ? incumbent_feature->value_core->profile()
                            : service_feature.value_core->profile();
                    plan.value_full_model = TrainFullValueModel(profile,
                                                                service_replay,
                                                                rebuild_context.next_model_version,
                                                                clock_spec_.delta,
                                                                clock_spec_.tz);
                    if (!plan.value_full_model) {
                        rebuild_result.candidate_state = "train_failed";
                        persist_result(rebuild_result, false);
                        return error::OK;
                    }
                    ++serviceable_feature_count;
                }
                rebuild_result.apply_plans.push_back(std::move(plan));
                continue;
            }

            RatioRebuildContext rebuild_context;
            if (incumbent_feature && incumbent_feature->ratio_core) {
                incumbent_feature->ratio_core->BuildRebuildContext(request.key, &rebuild_context);
            } else if (service_feature.ratio_core) {
                service_feature.ratio_core->BuildRebuildContext(request.key, &rebuild_context);
            }
            plan.model_version = rebuild_context.next_model_version;

            RatioReplaySeries service_replay;
            if (BuildRelationRatioReplay(
                    service_feature.spec,
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
            const std::size_t service_holdout =
                DecideRelationHoldoutCount(service_replay.points.size());
            const std::size_t service_train_count =
                service_replay.points.size() - service_holdout;
            plan.train_window =
                BuildReplayWindowSummary(service_replay, 0, service_train_count);
            plan.holdout_window = BuildReplayWindowSummary(service_replay,
                                                           service_train_count,
                                                           service_replay.points.size());
            if (!service_replay.points.empty()) {
                const RatioFeatureProfile& profile =
                    incumbent_feature && incumbent_feature->ratio_core
                        ? incumbent_feature->ratio_core->profile()
                        : service_feature.ratio_core->profile();
                plan.ratio_full_model = TrainFullRatioModel(profile,
                                                            service_replay,
                                                            rebuild_context.next_model_version,
                                                            clock_spec_.delta,
                                                            clock_spec_.tz);
                if (!plan.ratio_full_model) {
                    rebuild_result.candidate_state = "train_failed";
                    persist_result(rebuild_result, false);
                    return error::OK;
                }
                ++serviceable_feature_count;
            }
            rebuild_result.apply_plans.push_back(std::move(plan));
        }
    }

    rebuild_result.validation_feature_count = comparable_feature_count;
    if (comparable_feature_count > 0) {
        const CandidateValidationResult aggregate_validation =
            CandidateValidator::ValidateRelationAggregate(candidate_loss_sum,
                                                         incumbent_loss_sum,
                                                         comparable_feature_count);
        rebuild_result.candidate_loss = aggregate_validation.candidate_loss;
        rebuild_result.incumbent_loss = aggregate_validation.incumbent_loss;
        rebuild_result.validation_feature_count = aggregate_validation.validation_count;
        if (aggregate_validation.status != CandidateValidationStatus::kPassed) {
            rebuild_result.candidate_state = "validation_failed";
            persist_result(rebuild_result, false);
            return error::OK;
        }
        rebuild_result.switch_state = "formal_apply";
    } else {
        rebuild_result.switch_state = "direct_apply";
    }

    if (serviceable_feature_count == 0) {
        rebuild_result.candidate_state = "no_serviceable_feature";
        rebuild_result.switch_state = "none";
        persist_result(rebuild_result, false);
        return error::OK;
    }

    rebuild_result.candidate_state = "none";
    persist_result(rebuild_result, true);

    for (const auto& plan : rebuild_result.apply_plans) {
        std::shared_ptr<ValueDetectorCore> value_core;
        std::shared_ptr<RatioDetectorCore> ratio_core;
        {
            std::lock_guard<std::mutex> lock(runtime_mutex_);
            auto key_it = runtime_by_key_.find(request.key);
            if (key_it == runtime_by_key_.end()) continue;
            auto metric_it = key_it->second.metrics_by_name.find(plan.feature_spec.metric_name);
            if (metric_it == key_it->second.metrics_by_name.end()) continue;
            auto* routed_feature =
                FindRoutedFeatureRuntime(&metric_it->second, plan.feature_spec.feature);
            if (!routed_feature) continue;
            value_core = routed_feature->value_core;
            ratio_core = routed_feature->ratio_core;
        }

        if (plan.feature_spec.detector_kind == RelationRoutedDetectorKind::kValue) {
            if (!value_core) continue;

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
            value_core->ApplyFormalModel(request.key, apply_result);
            continue;
        }

        if (!ratio_core) continue;

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
        ratio_core->ApplyFormalModel(request.key, apply_result);
    }

    return error::OK;
}

int BaselineRelationTask::RequestRebuild(const BaselineStringRef& key,
                                         BaselineRebuildReason reason) {
    if (!key.data || key.size == 0) return error::BAD_REQUEST;

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

int BaselineRelationTask::SubmitBlock(const RelationObservationBlock& block, FusionResult* out) {
    if (!out) return error::BAD_REQUEST;
    *out = FusionResult{};

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (EnsureOpenLocked() != error::OK) {
            out->key = block.key;
            out->ts = block.bucket_id;
            return error::UNAVAILABLE;
        }
    }

    const int validate_rc = ValidateRelationBlock(spec_, block);
    if (validate_rc != error::OK) {
        out->key = block.key;
        out->ts = block.bucket_id;
        return validate_rc;
    }

    const std::string key = CopyKey(block.key);
    std::vector<RelationMetricRuntimeState> metric_runtimes(spec_.metrics.size());
    {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        auto& runtime_state = runtime_by_key_[key];
        ++runtime_state.seen_block_count;

        for (uint32_t metric_index = 0; metric_index < block.metric_count; ++metric_index) {
            const std::string& metric_name = spec_.metrics[metric_index];
            auto& metric_runtime = runtime_state.metrics_by_name[metric_name];
            if (metric_runtime.service_basis.group_space_id.empty()) {
                RelationServiceBasis bootstrap_basis;
                const auto build_input = MakeBootstrapBasisInput(spec_, block, metric_index);
                if (RelationBasisBuilder::BuildServiceBasis(build_input, &bootstrap_basis) ==
                    error::OK) {
                    metric_runtime.service_basis = std::move(bootstrap_basis);
                    RelationBasisBuilder::BuildEvalBasis(nullptr, spec_, &metric_runtime.eval_basis);
                }
            }

            if (metric_runtime.service_basis.group_space_id.empty()) {
                metric_runtime.service_basis.feature_base = spec_.feature_base;
                metric_runtime.service_basis.metric_name = metric_name;
                metric_runtime.service_basis.group_space_id = spec_.group_space_id;
                metric_runtime.service_basis.group_space_version =
                    spec_.group_space_version.value_or("");
                metric_runtime.service_basis.k_head = spec_.summary_policy.k_head;
            }

            MaterializeMetricRuntime(TaskId(),
                                     key,
                                     spec_,
                                     clock_spec_,
                                     event_calendar_spec_,
                                     source_resolver_,
                                     metric_name,
                                     &metric_runtime);
            metric_runtimes[metric_index] = metric_runtime;
        }
    }

    std::vector<RelationMetricSummary> summaries(spec_.metrics.size());
    for (uint32_t metric_index = 0; metric_index < block.metric_count; ++metric_index) {
        const int rc = RelationSummaryExtractor::ExtractMetricSummary(
            block,
            metric_index,
            metric_runtimes[metric_index].service_basis,
            &summaries[metric_index]);
        if (rc != error::OK) {
            out->key = block.key;
            out->ts = block.bucket_id;
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
    std::vector<FusionSingleContribution> fusion_singles;

    for (size_t metric_index = 0; metric_index < metric_runtimes.size(); ++metric_index) {
        const RelationMetricSummary& summary = summaries[metric_index];
        auto& metric_runtime = metric_runtimes[metric_index];
        for (const auto& routed_feature : metric_runtime.routed_features) {
            DetectorSubmitOutput submit_output;
            if (routed_feature.spec.detector_kind == RelationRoutedDetectorKind::kValue) {
                ValueObservation observation;
                if (!RelationRouter::BuildValueObservation(
                        routed_feature.spec,
                        block.key,
                        block.bucket_id,
                        summary,
                        &observation)) {
                    continue;
                }
                if (!routed_feature.value_core ||
                    routed_feature.value_core->Submit(observation, &submit_output) != error::OK) {
                    continue;
                }
            } else {
                RatioObservation observation;
                if (!RelationRouter::BuildRatioObservation(
                        routed_feature.spec,
                        block.key,
                        block.bucket_id,
                        summary,
                        &observation)) {
                    continue;
                }
                if (!routed_feature.ratio_core ||
                    routed_feature.ratio_core->Submit(observation, &submit_output) != error::OK) {
                    continue;
                }
            }

            {
                std::lock_guard<std::mutex> lock(runtime_mutex_);
                auto key_it = runtime_by_key_.find(key);
                if (key_it != runtime_by_key_.end()) {
                    auto metric_it =
                        key_it->second.metrics_by_name.find(metric_runtimes[metric_index]
                                                                .service_basis.metric_name);
                    if (metric_it != key_it->second.metrics_by_name.end()) {
                        auto* stored_feature = FindRoutedFeatureRuntime(
                            &metric_it->second, routed_feature.spec.feature);
                        if (stored_feature) {
                            stored_feature->last_detector_result =
                                CaptureDetectorResult(submit_output.detector_result);
                        }
                    }
                }
            }

            aggregate_flags |= submit_output.detector_result.flags;
            MergeFeatureResult(submit_output.detector_result, &has_serviceable, &merged_result);
            MergeRebuildIntent(submit_output,
                               &rebuild_required,
                               &rebuild_reason,
                               &rebuild_start_hint,
                               &rebuild_end);

            FusionSingleContribution single;
            single.local_slot = routed_feature.spec.local_slot;
            single.metric_name = routed_feature.spec.metric_name;
            single.summary_kind = routed_feature.spec.summary_kind;
            single.detector_result = submit_output.detector_result;
            fusion_singles.push_back(std::move(single));
        }
    }

    if (has_serviceable) {
        merged_result.flags |= aggregate_flags;
    }

    RelationPatternFusionOutput pattern_output;
    RelationPatternFusionInput pattern_input;
    pattern_input.key = key;
    pattern_input.bucket_id = block.bucket_id;
    pattern_input.feature_base = spec_.feature_base;
    pattern_input.singles = fusion_singles;
    const int fusion_rc = RelationPatternFusion::Compute(pattern_input, &pattern_output);
    if (fusion_rc != error::OK) {
        out->key = block.key;
        out->ts = block.bucket_id;
        return fusion_rc;
    }

    StoredFusionResult selected_fusion = pattern_output.fusion_result;
    MaterializeStoredFusionResult(selected_fusion, out);

    if (key_risk_fusion_) {
        for (const auto& single : fusion_singles) {
            key_risk_fusion_->UpdateSingleDetectorResult(
                block.bucket_id,
                FusionSourceId{TaskId(), FusionSourceKind::kRoutedSingle, single.local_slot},
                single.detector_result);
        }
        for (const auto& pattern : pattern_output.pattern_contributions) {
            const FusionResult pattern_result =
                BuildPatternContributionResult(key, block.bucket_id, pattern);
            key_risk_fusion_->UpdateRelationFusionResult(
                block.bucket_id,
                FusionSourceId{TaskId(), FusionSourceKind::kRelationPattern, pattern.local_slot},
                pattern_result);
        }

        KeyRiskFusionSnapshot snapshot;
        if (key_risk_fusion_->QueryKeyFusionSnapshot(key, &snapshot) == error::OK) {
            if (const StoredFusionResult* matched = SelectResultForBucket(snapshot,
                                                                          block.bucket_id)) {
                selected_fusion = *matched;
                MaterializeStoredFusionResult(selected_fusion, out);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(runtime_mutex_);
        auto& runtime_state = runtime_by_key_[key];
        runtime_state.last_fusion_result = selected_fusion;
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
                out->risk = std::max(out->risk, 0.0);
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
    if (rebuild_queue_) rebuild_queue_->CancelTask(TaskId());
    if (rebuild_runtime_) rebuild_runtime_->CloseAndWait();
    if (key_risk_fusion_) key_risk_fusion_->RemoveTaskContributions(TaskId());
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    runtime_by_key_.clear();
}

}  // namespace baseline
}  // namespace flowsql
