/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "value_task.h"

#include <limits>
#include <utility>

#include "plugins/baseline/rolling/rolling_task_runner.h"

namespace flowsql {
namespace baseline {

BaselineValueTask::BaselineValueTask(TaskRegistry* registry,
                                     std::string task_id,
                                     std::string task_name,
                                     std::string config_content,
                                     BaselineTaskSpec spec,
                                     std::shared_ptr<const CompiledEventCalendar> compiled_event_calendar)
    : BaselineTaskBase(registry,
                       std::move(task_id),
                       BaselineTaskKind::kValue,
                       std::move(task_name),
                       std::move(config_content)),
      spec_(std::move(spec)),
      compiled_event_calendar_(std::move(compiled_event_calendar)) {}

const char* BaselineValueTask::Id() const { return BaselineTaskBase::Id(); }
const char* BaselineValueTask::Name() const { return BaselineTaskBase::Name(); }
BaselineTaskKind BaselineValueTask::Kind() const { return BaselineTaskBase::Kind(); }

BaselineSerializationResult BaselineValueTask::ExportConfig(
    BaselineSerializationFormat format) const {
    return BaselineTaskBase::ExportConfig(format);
}

BaselineSerializationResult BaselineValueTask::QueryTaskSnapshot(
    BaselineSerializationFormat format) const {
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return {status, ""};
    return QueryRollingTaskSnapshot(spec_, rolling_states_, format);
}

BaselineSerializationResult BaselineValueTask::QuerySeriesSnapshot(
    std::string_view series_key,
    BaselineSerializationFormat format) const {
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return {status, ""};
    return QueryRollingSeriesSnapshot(spec_, rolling_states_, series_key, format);
}

BaselineStatus BaselineValueTask::Close() { return BaselineTaskBase::Close(); }

RollingBaselineResult BaselineValueTask::SubmitObservation(
    const ValueRollingObservation& obs,
    const RollingSubmitOptions& options) {
    RollingBaselineResult result;
    result.series_key = obs.series_key;
    result.bucket_id = obs.bucket_id;
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) {
        result.status = status;
        return result;
    }
    return RunValueRollingSubmit(
        spec_, seeds_by_series_, &rolling_states_, obs, options);
}

RollingPrediction BaselineValueTask::PredictRolling(std::string_view series_key,
                                                    int64_t bucket_id) const {
    RollingPrediction result;
    result.series_key = std::string(series_key);
    result.bucket_id = bucket_id;
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) {
        result.status = status;
        return result;
    }
    return PredictRollingForSeries(spec_, seeds_by_series_, rolling_states_, series_key, bucket_id);
}

RollingPredictionSequence BaselineValueTask::PredictRolling(
    std::string_view series_key,
    int64_t start_bucket_id,
    uint32_t point_count) const {
    RollingPredictionSequence sequence;
    sequence.series_key = std::string(series_key);
    sequence.start_bucket_id = start_bucket_id;
    sequence.point_count = point_count;
    if (point_count == 0) {
        sequence.status = BaselineStatus::kInvalidArgument;
        return sequence;
    }
    if (start_bucket_id > std::numeric_limits<int64_t>::max() -
                              static_cast<int64_t>(point_count - 1)) {
        sequence.status = BaselineStatus::kInvalidArgument;
        return sequence;
    }

    sequence.predictions.reserve(point_count);
    for (uint32_t i = 0; i < point_count; ++i) {
        RollingPrediction prediction =
            PredictRolling(series_key, start_bucket_id + static_cast<int64_t>(i));
        if (sequence.status == BaselineStatus::kOk &&
            prediction.status != BaselineStatus::kOk) {
            sequence.status = prediction.status;
        }
        sequence.predictions.push_back(std::move(prediction));
    }
    return sequence;
}

BootstrapTrainResult BaselineValueTask::Bootstrap(const ValueBootstrapInput& input) {
    BootstrapTrainResult result;
    result.status = EnsureOpen();
    if (result.status != BaselineStatus::kOk) return result;
    if (!input.options.force_replace_existing_artifact &&
        FindBootstrapArtifact(artifacts_by_series_, input.series_key)) {
        result.status = BaselineStatus::kInvalidArgument;
        if (input.options.include_diagnostics) {
            result.diagnostics = "bootstrap artifact already exists for series_key";
        }
        return result;
    }
    BootstrapArtifact artifact;
    result = bootstrap_engine_.TrainValue(
        spec_, input, &artifact, compiled_event_calendar_.get());
    if (result.status == BaselineStatus::kOk) {
        const BaselineStatus seed_status =
            StoreBootstrapArtifact(input.series_key,
                                   std::move(artifact),
                                   bootstrap_engine_,
                                   &artifacts_by_series_,
                                   &seeds_by_series_);
        if (seed_status != BaselineStatus::kOk) {
            result.status = seed_status;
            return result;
        }
        (void)WarmupRollingStatesFromBootstrapSeeds(spec_, seeds_by_series_, &rolling_states_);
    }
    return result;
}

BootstrapPrediction BaselineValueTask::PredictBootstrap(
    std::string_view series_key,
    int64_t bucket_id,
    const BootstrapPredictionOptions& options) const {
    const std::string key(series_key);
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) {
        BootstrapPrediction prediction;
        prediction.status = status;
        prediction.series_key = key;
        prediction.bucket_id = bucket_id;
        return prediction;
    }
    const BootstrapArtifact* artifact = FindBootstrapArtifact(artifacts_by_series_, key);
    if (!artifact) {
        BootstrapPrediction prediction;
        prediction.status = key.empty() ? BaselineStatus::kInvalidArgument
                                        : BaselineStatus::kNotTrained;
        prediction.series_key = key;
        prediction.bucket_id = bucket_id;
        return prediction;
    }
    return bootstrap_engine_.PredictValue(
        *artifact, bucket_id, options, &spec_, compiled_event_calendar_.get());
}

BootstrapPredictionSequence BaselineValueTask::PredictBootstrap(
    std::string_view series_key,
    int64_t start_bucket_id,
    uint32_t point_count,
    const BootstrapPredictionOptions& options) const {
    BootstrapPredictionSequence sequence;
    sequence.series_key = std::string(series_key);
    sequence.start_bucket_id = start_bucket_id;
    sequence.point_count = point_count;
    if (point_count == 0) {
        sequence.status = BaselineStatus::kInvalidArgument;
        return sequence;
    }
    if (start_bucket_id > std::numeric_limits<int64_t>::max() -
                              static_cast<int64_t>(point_count - 1)) {
        sequence.status = BaselineStatus::kInvalidArgument;
        return sequence;
    }

    sequence.predictions.reserve(point_count);
    for (uint32_t i = 0; i < point_count; ++i) {
        BootstrapPrediction prediction =
            PredictBootstrap(series_key,
                             start_bucket_id + static_cast<int64_t>(i),
                             options);
        if (sequence.status == BaselineStatus::kOk &&
            prediction.status != BaselineStatus::kOk) {
            sequence.status = prediction.status;
        }
        sequence.predictions.push_back(std::move(prediction));
    }
    return sequence;
}

BaselineSerializationResult BaselineValueTask::ExportBootstrapArtifact(
    BaselineSerializationFormat format) const {
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return {status, ""};
    return ExportBootstrapArtifactStore(artifacts_by_series_, bootstrap_engine_, format);
}

BaselineStatus BaselineValueTask::LoadBootstrapArtifact(
    std::string_view content,
    BaselineSerializationFormat format) {
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return status;
    const BaselineStatus load_status =
        LoadBootstrapArtifactStore(content,
                                   format,
                                   bootstrap_engine_,
                                   spec_,
                                   BootstrapArtifactKind::kValue,
                                   &artifacts_by_series_,
                                   &seeds_by_series_);
    if (load_status == BaselineStatus::kOk) {
        (void)WarmupRollingStatesFromBootstrapSeeds(spec_, seeds_by_series_, &rolling_states_);
    }
    return load_status;
}

BaselineSerializationResult BaselineValueTask::ExportBootstrapSeed(
    BaselineSerializationFormat format) const {
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return {status, ""};
    return ExportBootstrapSeedStore(seeds_by_series_, bootstrap_engine_, format);
}

}  // namespace baseline
}  // namespace flowsql
