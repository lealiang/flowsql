/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "candidate_builder.h"

#include <algorithm>

namespace flowsql {
namespace baseline {

namespace {

constexpr std::size_t kMinTrainPointCount = 2;
constexpr std::size_t kMinReplayForHoldout = 3;
constexpr std::size_t kSwitchValidationTail = 16;

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

std::size_t DecideHoldoutCount(std::size_t total_count) {
    if (total_count < kMinReplayForHoldout) return 0;
    return std::min(kSwitchValidationTail, total_count / 2);
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
                                                  const EventCalendarSpec* event_calendar_spec,
                                                  ValueCandidateBuildResult* out) {
    if (!out) return CandidateBuildStatus::kTrainFailed;
    *out = ValueCandidateBuildResult{};
    out->replay_window = replay.window;

    if (!replay.window.has_data || replay.points.empty()) {
        return out->status = CandidateBuildStatus::kEmpty;
    }

    // candidate builder 只负责把 replay 切成 train / holdout，并把训练职责下沉给 trainer。
    // v1 的 holdout 固定取尾部，目的是让后续切换验证尽量贴近“最近阶段是否可服务”。
    const std::size_t holdout_count = DecideHoldoutCount(replay.points.size());
    const std::size_t train_count = replay.points.size() - holdout_count;

    if (train_count < kMinTrainPointCount) {
        return out->status = CandidateBuildStatus::kInsufficientTrainData;
    }

    out->train_window = BuildWindowSummary(replay, 0, train_count);
    out->holdout_window = BuildWindowSummary(replay, train_count, replay.points.size());

    ValueFormalTrainResult train_result;
    const ValueFormalTrainInput input{
        &profile,
        &replay,
        train_count,
        candidate_model_version,
        static_cast<uint64_t>(holdout_count),
        out->train_window,
        event_calendar_spec};
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
                                                  const EventCalendarSpec* event_calendar_spec,
                                                  RatioCandidateBuildResult* out) {
    if (!out) return CandidateBuildStatus::kTrainFailed;
    *out = RatioCandidateBuildResult{};
    out->replay_window = replay.window;

    if (!replay.window.has_data || replay.points.empty()) {
        return out->status = CandidateBuildStatus::kEmpty;
    }

    // T2 与 T1 共用相同的候选构建契约：先切尾部 holdout，再训练 train 段。
    const std::size_t holdout_count = DecideHoldoutCount(replay.points.size());
    const std::size_t train_count = replay.points.size() - holdout_count;

    if (train_count < kMinTrainPointCount) {
        return out->status = CandidateBuildStatus::kInsufficientTrainData;
    }

    out->train_window = BuildWindowSummary(replay, 0, train_count);
    out->holdout_window = BuildWindowSummary(replay, train_count, replay.points.size());

    RatioFormalTrainResult train_result;
    const RatioFormalTrainInput input{
        &profile,
        &replay,
        train_count,
        candidate_model_version,
        static_cast<uint64_t>(holdout_count),
        out->train_window,
        event_calendar_spec};
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

}  // namespace baseline
}  // namespace flowsql
