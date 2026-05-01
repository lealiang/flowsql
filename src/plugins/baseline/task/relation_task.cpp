/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "relation_task.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <utility>

namespace flowsql {
namespace baseline {

BaselineRelationTask::BaselineRelationTask(TaskRegistry* registry,
                         std::string task_id,
                         std::string task_name,
                         std::string config_content,
                         RelationTaskCreateSpec spec,
                         std::shared_ptr<const CompiledEventCalendar> compiled_event_calendar)
    : BaselineTaskBase(registry,
                       std::move(task_id),
                       BaselineTaskKind::kRelation,
                       std::move(task_name),
                       std::move(config_content)),
      spec_(std::move(spec)),
      compiled_event_calendar_(std::move(compiled_event_calendar)) {}

const char* BaselineRelationTask::Id() const { return BaselineTaskBase::Id(); }
const char* BaselineRelationTask::Name() const { return BaselineTaskBase::Name(); }
BaselineTaskKind BaselineRelationTask::Kind() const { return BaselineTaskBase::Kind(); }

BaselineSerializationResult BaselineRelationTask::ExportConfig(
    BaselineSerializationFormat format) const {
    return BaselineTaskBase::ExportConfig(format);
}

BaselineSerializationResult BaselineRelationTask::QueryTaskSnapshot(
    BaselineSerializationFormat format) const {
    return BaselineTaskBase::QueryTaskSnapshot(format);
}

BaselineSerializationResult BaselineRelationTask::QuerySeriesSnapshot(
    std::string_view series_key,
    BaselineSerializationFormat format) const {
    return BaselineTaskBase::QuerySeriesSnapshot(series_key, format);
}

BaselineStatus BaselineRelationTask::Close() { return BaselineTaskBase::Close(); }

BootstrapTrainResult BaselineRelationTask::Bootstrap(const RelationBootstrapInput& input) {
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
    result = bootstrap_engine_.TrainRelation(
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

BaselineSerializationResult BaselineRelationTask::ExportBootstrapArtifact(
    BaselineSerializationFormat format) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const BaselineStatus status = EnsureOpenLocked();
    if (status != BaselineStatus::kOk) return {status, ""};
    return ExportBootstrapArtifactStore(artifacts_by_series_, bootstrap_engine_, format);
}

BaselineStatus BaselineRelationTask::LoadBootstrapArtifact(
    std::string_view content,
    BaselineSerializationFormat format) {
    std::lock_guard<std::mutex> lock(mutex_);
    const BaselineStatus status = EnsureOpenLocked();
    if (status != BaselineStatus::kOk) return status;
    return LoadRelationBootstrapArtifactStore(content,
                                              format,
                                              bootstrap_engine_,
                                              spec_,
                                              &artifacts_by_series_,
                                              &seeds_by_series_);
}

BaselineSerializationResult BaselineRelationTask::ExportBootstrapSeed(
    BaselineSerializationFormat format) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const BaselineStatus status = EnsureOpenLocked();
    if (status != BaselineStatus::kOk) return {status, ""};
    return ExportBootstrapSeedStore(seeds_by_series_, bootstrap_engine_, format);
}

BaselineSerializationResult BaselineRelationTask::QueryBootstrapBasis(
    BaselineSerializationFormat format) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const BaselineStatus status = EnsureOpenLocked();
    if (status != BaselineStatus::kOk) return {status, ""};
    if (format != BaselineSerializationFormat::kJson) {
        return {BaselineStatus::kUnsupportedFormat, ""};
    }
    if (artifacts_by_series_.empty()) {
        return {BaselineStatus::kNotTrained, ""};
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("series_bases");
    writer.StartArray();
    for (const BootstrapArtifact* artifact : SortedBootstrapArtifacts(artifacts_by_series_)) {
        if (!artifact) continue;
        writer.StartObject();
        writer.Key("series_identity");
        writer.StartObject();
        writer.Key("series_key");
        writer.String(artifact->series_key.c_str());
        writer.EndObject();
        writer.Key("basis_by_metric");
        writer.StartArray();
        for (const auto& basis : artifact->relation_basis_by_metric) {
            writer.StartObject();
            writer.Key("metric");
            writer.String(basis.metric_name.c_str());
            writer.Key("support_explicit");
            writer.StartArray();
            for (uint32_t group_idx : basis.support_explicit) writer.Uint(group_idx);
            writer.EndArray();
            writer.Key("stable_head");
            writer.StartArray();
            for (uint32_t group_idx : basis.stable_head) writer.Uint(group_idx);
            writer.EndArray();
            writer.EndObject();
        }
        writer.EndArray();
        writer.EndObject();
    }
    writer.EndArray();
    writer.EndObject();
    return {BaselineStatus::kOk, buf.GetString()};
}

}  // namespace baseline
}  // namespace flowsql
