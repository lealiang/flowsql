/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "candidate_builder.h"

#include <common/error_code.h>

#include <algorithm>

#include "plugins/baseline/model/profile_config.h"

namespace flowsql {
namespace baseline {

namespace {

constexpr std::size_t kMinTrainPointCount = 2;

CandidateBuildStatus MapTrainFailure(FormalTrainFailureCode code) {
    switch (code) {
        case FormalTrainFailureCode::kNone:
            return CandidateBuildStatus::kTrained;
        case FormalTrainFailureCode::kInsufficientTrainData:
            return CandidateBuildStatus::kInsufficientTrainData;
        case FormalTrainFailureCode::kSolverUnavailable:
            return CandidateBuildStatus::kSolverUnavailable;
        case FormalTrainFailureCode::kTrainFailed:
            return CandidateBuildStatus::kTrainFailed;
    }
    return CandidateBuildStatus::kTrainFailed;
}

template <typename TSeries>
ReplayWindowSummary BuildWindowSummary(const TSeries& series,
                                       std::size_t begin,
                                       std::size_t end) {
    ReplayWindowSummary summary;
    if (begin >= end || end > series.points.size()) return summary;

    summary.has_data = true;
    summary.observation_count = static_cast<uint64_t>(end - begin);
    summary.first_bucket_id = series.points[begin].bucket_id;
    summary.last_bucket_id = series.points[end - 1].bucket_id;
    summary.request_bucket_start = summary.first_bucket_id;
    summary.request_bucket_end = summary.last_bucket_id;
    return summary;
}

std::vector<std::size_t> CollectValueValidIndices(const ValueFeatureProfile& profile,
                                                  const ValueReplaySeries& replay) {
    std::vector<std::size_t> indices;
    indices.reserve(replay.points.size());
    for (std::size_t i = 0; i < replay.points.size(); ++i) {
        if (profile.is_t1b && replay.points[i].sample_count < profile.n_train_min) continue;
        indices.push_back(i);
    }
    return indices;
}

std::vector<std::size_t> CollectRatioValidIndices(const RatioFeatureProfile& profile,
                                                  const RatioReplaySeries& replay) {
    std::vector<std::size_t> indices;
    indices.reserve(replay.points.size());
    for (std::size_t i = 0; i < replay.points.size(); ++i) {
        if (replay.points[i].denominator < static_cast<double>(profile.d_min_train)) continue;
        indices.push_back(i);
    }
    return indices;
}

template <typename TSeries>
std::size_t HoldoutStartIndex(const TSeries& series,
                              const std::vector<std::size_t>& valid_indices,
                              std::size_t holdout_valid_count) {
    if (holdout_valid_count == 0 || valid_indices.empty()) return series.points.size();
    return valid_indices[valid_indices.size() - holdout_valid_count];
}

}  // namespace

const char* CandidateBuildStatusName(CandidateBuildStatus status) {
    switch (status) {
        case CandidateBuildStatus::kNone:
            return "none";
        case CandidateBuildStatus::kTrained:
            return "trained";
        case CandidateBuildStatus::kEmpty:
            return "empty";
        case CandidateBuildStatus::kInsufficientTrainData:
            return "insufficient_train_data";
        case CandidateBuildStatus::kSolverUnavailable:
            return "solver_unavailable";
        case CandidateBuildStatus::kTrainFailed:
            return "train_failed";
    }
    return "train_failed";
}

CandidateBuildStatus CandidateBuilder::BuildValue(const ValueFeatureProfile& profile,
                                                  const ValueReplaySeries& replay,
                                                  uint64_t candidate_model_version,
                                                  const BaselineTaskSpec* task_spec,
                                                  int64_t delta,
                                                  const std::string& tz,
                                                  const CompiledEventCalendar* compiled_event_calendar,
                                                  ValueCandidateBuildResult* out) {
    if (!out) return CandidateBuildStatus::kTrainFailed;
    *out = ValueCandidateBuildResult{};
    out->replay_window = replay.window;

    if (!replay.window.has_data || replay.points.empty()) {
        return out->status = CandidateBuildStatus::kEmpty;
    }

    const SharedProfileConfig shared_config = DefaultSharedProfileConfig();
    const std::vector<std::size_t> valid_indices = CollectValueValidIndices(profile, replay);
    const std::size_t holdout_valid_count =
        valid_indices.size() >= 2 * shared_config.n_val_switch ? shared_config.n_val_switch : 0;
    const std::size_t holdout_begin =
        HoldoutStartIndex(replay, valid_indices, holdout_valid_count);
    const std::size_t train_count = holdout_begin;

    if (train_count < kMinTrainPointCount) {
        return out->status = CandidateBuildStatus::kInsufficientTrainData;
    }

    out->train_window = BuildWindowSummary(replay, 0, train_count);
    out->holdout_window = BuildWindowSummary(replay, holdout_begin, replay.points.size());

    ValueFormalTrainResult train_result;
    const ValueFormalTrainInput input{
        &profile,
        &replay,
        train_count,
        candidate_model_version,
        static_cast<uint64_t>(holdout_valid_count),
        out->train_window,
        task_spec,
        delta,
        tz,
        compiled_event_calendar};
    out->status = MapTrainFailure(FormalModelTrainer::TrainValue(input, &train_result));
    if (out->status != CandidateBuildStatus::kTrained || !train_result.model) {
        if (out->status == CandidateBuildStatus::kTrained) {
            out->status = CandidateBuildStatus::kTrainFailed;
        }
        return out->status;
    }

    out->candidate_model_version = candidate_model_version;
    out->candidate_model = std::move(train_result.model);
    return out->status = CandidateBuildStatus::kTrained;
}

CandidateBuildStatus CandidateBuilder::BuildRatio(const RatioFeatureProfile& profile,
                                                  const RatioReplaySeries& replay,
                                                  uint64_t candidate_model_version,
                                                  const BaselineTaskSpec* task_spec,
                                                  int64_t delta,
                                                  const std::string& tz,
                                                  const CompiledEventCalendar* compiled_event_calendar,
                                                  RatioCandidateBuildResult* out) {
    if (!out) return CandidateBuildStatus::kTrainFailed;
    *out = RatioCandidateBuildResult{};
    out->replay_window = replay.window;

    if (!replay.window.has_data || replay.points.empty()) {
        return out->status = CandidateBuildStatus::kEmpty;
    }

    const SharedProfileConfig shared_config = DefaultSharedProfileConfig();
    const std::vector<std::size_t> valid_indices = CollectRatioValidIndices(profile, replay);
    const std::size_t holdout_valid_count =
        valid_indices.size() >= 2 * shared_config.n_val_switch ? shared_config.n_val_switch : 0;
    const std::size_t holdout_begin =
        HoldoutStartIndex(replay, valid_indices, holdout_valid_count);
    const std::size_t train_count = holdout_begin;

    if (train_count < kMinTrainPointCount) {
        return out->status = CandidateBuildStatus::kInsufficientTrainData;
    }

    out->train_window = BuildWindowSummary(replay, 0, train_count);
    out->holdout_window = BuildWindowSummary(replay, holdout_begin, replay.points.size());

    RatioFormalTrainResult train_result;
    const RatioFormalTrainInput input{
        &profile,
        &replay,
        train_count,
        candidate_model_version,
        static_cast<uint64_t>(holdout_valid_count),
        out->train_window,
        task_spec,
        delta,
        tz,
        compiled_event_calendar};
    out->status = MapTrainFailure(FormalModelTrainer::TrainRatio(input, &train_result));
    if (out->status != CandidateBuildStatus::kTrained || !train_result.model) {
        if (out->status == CandidateBuildStatus::kTrained) {
            out->status = CandidateBuildStatus::kTrainFailed;
        }
        return out->status;
    }

    out->candidate_model_version = candidate_model_version;
    out->candidate_model = std::move(train_result.model);
    return out->status = CandidateBuildStatus::kTrained;
}

CandidateBuildStatus CandidateBuilder::BuildRelationMetricBases(
    const RelationBasisBuildInput& input,
    const RelationServiceBasis* incumbent_basis,
    const RelationTaskSpec& task_spec,
    RelationMetricCandidateBuildResult* out) {
    if (!out) return CandidateBuildStatus::kTrainFailed;
    *out = RelationMetricCandidateBuildResult{};

    if (RelationBasisBuilder::BuildServiceBasis(input, &out->candidate_service_basis) !=
        error::OK) {
        return out->status = CandidateBuildStatus::kTrainFailed;
    }

    if (RelationBasisBuilder::BuildEvalBasis(incumbent_basis,
                                             task_spec,
                                             &out->candidate_eval_basis) != error::OK) {
        *out = RelationMetricCandidateBuildResult{};
        return out->status = CandidateBuildStatus::kTrainFailed;
    }

    out->lineage_compatibility = out->candidate_eval_basis.compatibility;
    return out->status = CandidateBuildStatus::kTrained;
}

}  // namespace baseline
}  // namespace flowsql
