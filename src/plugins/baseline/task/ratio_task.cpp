/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "ratio_task.h"

#include <utility>

#include "plugins/baseline/rolling/rolling_task_runner.h"

namespace flowsql {
namespace baseline {

BaselineRatioTask::BaselineRatioTask(TaskRegistry* registry,
                                     std::string task_id,
                                     std::string task_name,
                                     std::string config_content,
                                     BaselineTaskSpec spec,
                                     std::shared_ptr<const CompiledEventCalendar> compiled_event_calendar)
    : BaselineTaskBase(registry,
                       std::move(task_id),
                       BaselineTaskKind::kRatio,
                       std::move(task_name),
                       std::move(config_content)),
      spec_(std::move(spec)),
      compiled_event_calendar_(std::move(compiled_event_calendar)) {}

const char* BaselineRatioTask::Id() const { return BaselineTaskBase::Id(); }
const char* BaselineRatioTask::Name() const { return BaselineTaskBase::Name(); }
BaselineTaskKind BaselineRatioTask::Kind() const { return BaselineTaskBase::Kind(); }

BaselineSerializationResult BaselineRatioTask::ExportConfig(
    BaselineSerializationFormat format) const {
    return BaselineTaskBase::ExportConfig(format);
}

BaselineSerializationResult BaselineRatioTask::QueryTaskSnapshot(
    BaselineSerializationFormat format) const {
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return {status, ""};
    return QueryRollingTaskSnapshot(spec_, rolling_states_, format);
}

BaselineSerializationResult BaselineRatioTask::QuerySeriesSnapshot(
    std::string_view series_key,
    BaselineSerializationFormat format) const {
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return {status, ""};
    return QueryRollingSeriesSnapshot(spec_, rolling_states_, series_key, format);
}

BaselineStatus BaselineRatioTask::Close() { return BaselineTaskBase::Close(); }

RollingBaselineResult BaselineRatioTask::SubmitObservation(
    const RatioRollingObservation& obs,
    const RollingSubmitOptions& options) {
    RollingBaselineResult result;
    result.series_key = obs.series_key;
    result.bucket_id = obs.bucket_id;
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) {
        result.status = status;
        result.numerator = obs.numerator;
        result.denominator = obs.denominator;
        return result;
    }
    return RunRatioRollingSubmit(
        spec_, seeds_by_series_, &rolling_states_, obs, options);
}

RollingPrediction BaselineRatioTask::PredictRolling(std::string_view series_key,
                                                    int64_t bucket_id) const {
    RollingPrediction result;
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) {
        result.status = status;
        return result;
    }
    return PredictRollingForSeries(spec_, seeds_by_series_, rolling_states_, series_key, bucket_id);
}

BootstrapTrainResult BaselineRatioTask::Bootstrap(const RatioBootstrapInput& input) {
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
    result = bootstrap_engine_.TrainRatio(
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

BootstrapPrediction BaselineRatioTask::PredictBootstrap(
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
    return bootstrap_engine_.PredictRatio(
        *artifact, bucket_id, options, &spec_, compiled_event_calendar_.get());
}

BaselineSerializationResult BaselineRatioTask::ExportBootstrapArtifact(
    BaselineSerializationFormat format) const {
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return {status, ""};
    return ExportBootstrapArtifactStore(artifacts_by_series_, bootstrap_engine_, format);
}

BaselineStatus BaselineRatioTask::LoadBootstrapArtifact(
    std::string_view content,
    BaselineSerializationFormat format) {
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return status;
    const BaselineStatus load_status =
        LoadBootstrapArtifactStore(content,
                                   format,
                                   bootstrap_engine_,
                                   spec_,
                                   BootstrapArtifactKind::kRatio,
                                   &artifacts_by_series_,
                                   &seeds_by_series_);
    if (load_status == BaselineStatus::kOk) {
        (void)WarmupRollingStatesFromBootstrapSeeds(spec_, seeds_by_series_, &rolling_states_);
    }
    return load_status;
}

BaselineSerializationResult BaselineRatioTask::ExportBootstrapSeed(
    BaselineSerializationFormat format) const {
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return {status, ""};
    return ExportBootstrapSeedStore(seeds_by_series_, bootstrap_engine_, format);
}

}  // namespace baseline
}  // namespace flowsql
