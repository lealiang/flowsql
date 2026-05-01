/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "ratio_task.h"

#include <utility>

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
    return BaselineTaskBase::QueryTaskSnapshot(format);
}

BaselineSerializationResult BaselineRatioTask::QuerySeriesSnapshot(
    std::string_view series_key,
    BaselineSerializationFormat format) const {
    return BaselineTaskBase::QuerySeriesSnapshot(series_key, format);
}

BaselineStatus BaselineRatioTask::Close() { return BaselineTaskBase::Close(); }

BootstrapTrainResult BaselineRatioTask::Bootstrap(const RatioBootstrapInput& input) {
    std::lock_guard<std::mutex> lock(mutex_);
    BootstrapTrainResult result;
    result.status = EnsureOpenLocked();
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
    }
    return result;
}

BootstrapPrediction BaselineRatioTask::PredictBootstrap(
    std::string_view series_key,
    int64_t bucket_id,
    const BootstrapPredictionOptions& options) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key(series_key);
    const BaselineStatus status = EnsureOpenLocked();
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
    std::lock_guard<std::mutex> lock(mutex_);
    const BaselineStatus status = EnsureOpenLocked();
    if (status != BaselineStatus::kOk) return {status, ""};
    return ExportBootstrapArtifactStore(artifacts_by_series_, bootstrap_engine_, format);
}

BaselineStatus BaselineRatioTask::LoadBootstrapArtifact(
    std::string_view content,
    BaselineSerializationFormat format) {
    std::lock_guard<std::mutex> lock(mutex_);
    const BaselineStatus status = EnsureOpenLocked();
    if (status != BaselineStatus::kOk) return status;
    return LoadBootstrapArtifactStore(content,
                                      format,
                                      bootstrap_engine_,
                                      spec_,
                                      BootstrapArtifactKind::kRatio,
                                      &artifacts_by_series_,
                                      &seeds_by_series_);
}

BaselineSerializationResult BaselineRatioTask::ExportBootstrapSeed(
    BaselineSerializationFormat format) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const BaselineStatus status = EnsureOpenLocked();
    if (status != BaselineStatus::kOk) return {status, ""};
    return ExportBootstrapSeedStore(seeds_by_series_, bootstrap_engine_, format);
}

}  // namespace baseline
}  // namespace flowsql
