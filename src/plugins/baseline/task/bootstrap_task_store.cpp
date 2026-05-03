/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "bootstrap_task_store.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>

#include "plugins/baseline/serialization/json_serialization.h"

namespace flowsql {
namespace baseline {
namespace {

std::string SerializeJsonValue(const rapidjson::Value& value) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    value.Accept(writer);
    return buf.GetString();
}

std::vector<std::string> SortedBootstrapStoreKeys(
    const BootstrapArtifactStore& store) {
    std::vector<std::string> keys;
    keys.reserve(store.size());
    for (const auto& entry : store) keys.push_back(entry.first);
    std::sort(keys.begin(), keys.end());
    return keys;
}

std::vector<std::string> SortedBootstrapSeedKeys(const BootstrapSeedStore& store) {
    std::vector<std::string> keys;
    keys.reserve(store.size());
    for (const auto& entry : store) keys.push_back(entry.first);
    std::sort(keys.begin(), keys.end());
    return keys;
}

bool SameTaskIdentity(const BootstrapTaskIdentity& lhs,
                      const BootstrapTaskIdentity& rhs) {
    return lhs.task_id == rhs.task_id &&
           lhs.task_kind == rhs.task_kind &&
           lhs.feature_type == rhs.feature_type &&
           lhs.feature_id == rhs.feature_id &&
           lhs.profile == rhs.profile;
}

bool SameClockSpec(const BootstrapClockSpec& lhs, const BootstrapClockSpec& rhs) {
    return lhs.bucket_seconds == rhs.bucket_seconds &&
           lhs.timezone == rhs.timezone;
}

bool SameCalendarRef(const BootstrapCalendarRef& lhs,
                     const BootstrapCalendarRef& rhs) {
    return lhs.calendar_id == rhs.calendar_id &&
           lhs.calendar_version == rhs.calendar_version;
}

BaselineStatus ValidateTopLevelMetadata(const rapidjson::Document& doc,
                                        const BootstrapArtifact& first_artifact) {
    if (doc.HasMember("artifact_kind")) {
        if (!doc["artifact_kind"].IsString() ||
            std::string(doc["artifact_kind"].GetString()) !=
                ArtifactKindName(first_artifact.artifact_kind)) {
            return BaselineStatus::kIncompatibleArtifact;
        }
    }

    BootstrapTaskIdentity top_identity;
    if (doc.HasMember("task_identity")) {
        if (!ReadTaskIdentity(doc, &top_identity) ||
            !SameTaskIdentity(top_identity, first_artifact.task_identity)) {
            return BaselineStatus::kIncompatibleArtifact;
        }
    }

    BootstrapClockSpec top_clock;
    if (doc.HasMember("clock_spec")) {
        if (!ReadClockSpec(doc, &top_clock) ||
            !SameClockSpec(top_clock, first_artifact.clock_spec)) {
            return BaselineStatus::kIncompatibleArtifact;
        }
    }

    BootstrapCalendarRef top_calendar;
    if (doc.HasMember("calendar_ref")) {
        if (!ReadCalendarRef(doc, &top_calendar) ||
            !SameCalendarRef(top_calendar, first_artifact.calendar_ref)) {
            return BaselineStatus::kIncompatibleArtifact;
        }
    }

    return BaselineStatus::kOk;
}

BaselineStatus ValidateTopLevelMetadataAcrossSeries(
    const BootstrapArtifact& first_artifact,
    const BootstrapArtifact& artifact) {
    if (artifact.artifact_kind != first_artifact.artifact_kind ||
        !SameTaskIdentity(artifact.task_identity, first_artifact.task_identity) ||
        !SameClockSpec(artifact.clock_spec, first_artifact.clock_spec) ||
        !SameCalendarRef(artifact.calendar_ref, first_artifact.calendar_ref)) {
        return BaselineStatus::kIncompatibleArtifact;
    }
    return BaselineStatus::kOk;
}

template <typename ValidateArtifact>
BaselineStatus LoadBootstrapArtifactStoreInternal(
    std::string_view content,
    BaselineSerializationFormat format,
    const BootstrapEngine& engine,
    ValidateArtifact validate_artifact,
    BootstrapArtifactStore* artifacts,
    BootstrapSeedStore* seeds) {
    if (!artifacts || !seeds) return BaselineStatus::kInvalidArgument;
    if (format != BaselineSerializationFormat::kJson) {
        return BaselineStatus::kUnsupportedFormat;
    }

    rapidjson::Document doc;
    doc.Parse(content.data(), content.size());
    if (doc.HasParseError() || !doc.IsObject()) return BaselineStatus::kParseFailed;
    if (!doc.HasMember("document_kind") || !doc["document_kind"].IsString() ||
        std::string(doc["document_kind"].GetString()) != "bootstrap_artifact") {
        return BaselineStatus::kIncompatibleArtifact;
    }
    if (!doc.HasMember("schema_version") || !doc["schema_version"].IsInt() ||
        doc["schema_version"].GetInt() != 1) {
        return BaselineStatus::kIncompatibleArtifact;
    }
    if (!doc.HasMember("algorithm_version") || !doc["algorithm_version"].IsString() ||
        std::string(doc["algorithm_version"].GetString()) != "b1-bootstrap-v1") {
        return BaselineStatus::kIncompatibleArtifact;
    }
    if (!doc.HasMember("series_artifacts") || !doc["series_artifacts"].IsArray()) {
        return BaselineStatus::kParseFailed;
    }

    BootstrapArtifactStore loaded_artifacts;
    BootstrapSeedStore loaded_seeds;
    std::unordered_set<std::string> seen_series;
    BootstrapArtifact first_artifact;
    bool has_first_artifact = false;
    for (const auto& item : doc["series_artifacts"].GetArray()) {
        if (!item.IsObject()) return BaselineStatus::kParseFailed;
        BootstrapArtifact artifact;
        const BaselineStatus load_status =
            engine.LoadArtifact(SerializeJsonValue(item), format, &artifact);
        if (load_status != BaselineStatus::kOk) return load_status;
        if (artifact.series_key.empty()) return BaselineStatus::kIncompatibleArtifact;
        if (!seen_series.insert(artifact.series_key).second) {
            return BaselineStatus::kIncompatibleArtifact;
        }
        const BaselineStatus compatibility = validate_artifact(artifact);
        if (compatibility != BaselineStatus::kOk) return compatibility;
        if (!has_first_artifact) {
            first_artifact = artifact;
            has_first_artifact = true;
            const BaselineStatus top_status = ValidateTopLevelMetadata(doc, first_artifact);
            if (top_status != BaselineStatus::kOk) return top_status;
        } else {
            const BaselineStatus top_status =
                ValidateTopLevelMetadataAcrossSeries(first_artifact, artifact);
            if (top_status != BaselineStatus::kOk) return top_status;
        }

        BootstrapSeed seed;
        const BaselineStatus seed_status = engine.ExportSeed(artifact, &seed);
        if (seed_status != BaselineStatus::kOk) return seed_status;
        loaded_seeds.emplace(artifact.series_key, std::move(seed));
        loaded_artifacts.emplace(artifact.series_key, std::move(artifact));
    }
    if (loaded_artifacts.empty()) return BaselineStatus::kNotTrained;

    *artifacts = std::move(loaded_artifacts);
    *seeds = std::move(loaded_seeds);
    return BaselineStatus::kOk;
}

}  // namespace

BaselineStatus StoreBootstrapArtifact(std::string_view series_key,
                                      BootstrapArtifact artifact,
                                      const BootstrapEngine& engine,
                                      BootstrapArtifactStore* artifacts,
                                      BootstrapSeedStore* seeds) {
    if (!artifacts || !seeds || series_key.empty()) {
        return BaselineStatus::kInvalidArgument;
    }
    BootstrapSeed seed;
    const BaselineStatus seed_status = engine.ExportSeed(artifact, &seed);
    if (seed_status != BaselineStatus::kOk) return seed_status;
    const std::string key(series_key);
    artifacts->insert_or_assign(key, std::move(artifact));
    seeds->insert_or_assign(key, std::move(seed));
    return BaselineStatus::kOk;
}

const BootstrapArtifact* FindBootstrapArtifact(
    const BootstrapArtifactStore& artifacts,
    std::string_view series_key) {
    const auto it = artifacts.find(std::string(series_key));
    return it == artifacts.end() ? nullptr : &it->second;
}

std::vector<const BootstrapArtifact*> SortedBootstrapArtifacts(
    const BootstrapArtifactStore& artifacts) {
    std::vector<const BootstrapArtifact*> values;
    for (const auto& key : SortedBootstrapStoreKeys(artifacts)) {
        const auto it = artifacts.find(key);
        if (it != artifacts.end()) values.push_back(&it->second);
    }
    return values;
}

BaselineSerializationResult ExportBootstrapArtifactStore(
    const BootstrapArtifactStore& artifacts,
    const BootstrapEngine& engine,
    BaselineSerializationFormat format) {
    if (format != BaselineSerializationFormat::kJson) {
        return {BaselineStatus::kUnsupportedFormat, ""};
    }
    if (artifacts.empty()) return {BaselineStatus::kNotTrained, ""};

    const auto keys = SortedBootstrapStoreKeys(artifacts);
    const auto first = artifacts.find(keys.front());
    if (first == artifacts.end()) return {BaselineStatus::kSerializationFailed, ""};

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("schema_version");
    writer.Int(1);
    WriteStringField(&writer, "document_kind", "bootstrap_artifact");
    WriteStringField(&writer, "algorithm_version", "b1-bootstrap-v1");
    WriteStringField(&writer, "artifact_kind", ArtifactKindName(first->second.artifact_kind));
    WriteTaskIdentity(&writer, first->second.task_identity);
    WriteClockSpec(&writer, first->second.clock_spec);
    WriteCalendarRef(&writer, first->second.calendar_ref);
    writer.Key("series_artifacts");
    writer.StartArray();
    for (const auto& key : keys) {
        const auto it = artifacts.find(key);
        if (it == artifacts.end()) continue;
        auto [status, json] = engine.ExportArtifact(it->second, format);
        if (status != BaselineStatus::kOk) return {status, ""};
        rapidjson::Document item;
        item.Parse(json.c_str());
        if (item.HasParseError() || !item.IsObject()) {
            return {BaselineStatus::kSerializationFailed, ""};
        }
        item.Accept(writer);
    }
    writer.EndArray();
    writer.EndObject();
    return {BaselineStatus::kOk, buf.GetString()};
}

BaselineSerializationResult ExportBootstrapSeedStore(
    const BootstrapSeedStore& seeds,
    const BootstrapEngine& engine,
    BaselineSerializationFormat format) {
    if (format != BaselineSerializationFormat::kJson) {
        return {BaselineStatus::kUnsupportedFormat, ""};
    }
    if (seeds.empty()) return {BaselineStatus::kNotTrained, ""};

    const auto keys = SortedBootstrapSeedKeys(seeds);
    const auto first = seeds.find(keys.front());
    if (first == seeds.end()) return {BaselineStatus::kSerializationFailed, ""};

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("schema_version");
    writer.Int(1);
    WriteStringField(&writer, "document_kind", "bootstrap_seed");
    WriteStringField(&writer, "algorithm_version", "b1-bootstrap-v1");
    WriteStringField(&writer, "artifact_kind", ArtifactKindName(first->second.artifact_kind));
    WriteTaskIdentity(&writer, first->second.task_identity);
    WriteClockSpec(&writer, first->second.clock_spec);
    WriteCalendarRef(&writer, first->second.calendar_ref);
    writer.Key("series_seeds");
    writer.StartArray();
    for (const auto& key : keys) {
        const auto it = seeds.find(key);
        if (it == seeds.end()) continue;
        auto [status, json] = engine.ExportSeed(it->second, format);
        if (status != BaselineStatus::kOk) return {status, ""};
        rapidjson::Document item;
        item.Parse(json.c_str());
        if (item.HasParseError() || !item.IsObject()) {
            return {BaselineStatus::kSerializationFailed, ""};
        }
        item.Accept(writer);
    }
    writer.EndArray();
    writer.EndObject();
    return {BaselineStatus::kOk, buf.GetString()};
}

BaselineStatus LoadBootstrapArtifactStore(
    std::string_view content,
    BaselineSerializationFormat format,
    const BootstrapEngine& engine,
    const BaselineTaskSpec& spec,
    BootstrapArtifactKind expected_kind,
    BootstrapArtifactStore* artifacts,
    BootstrapSeedStore* seeds) {
    return LoadBootstrapArtifactStoreInternal(
        content,
        format,
        engine,
        [&engine, &spec, expected_kind](const BootstrapArtifact& artifact) {
            return engine.ValidateArtifactCompatibility(artifact, spec, expected_kind);
        },
        artifacts,
        seeds);
}

BaselineStatus LoadRelationBootstrapArtifactStore(
    std::string_view content,
    BaselineSerializationFormat format,
    const BootstrapEngine& engine,
    const RelationTaskCreateSpec& spec,
    BootstrapArtifactStore* artifacts,
    BootstrapSeedStore* seeds) {
    return LoadBootstrapArtifactStoreInternal(
        content,
        format,
        engine,
        [&engine, &spec](const BootstrapArtifact& artifact) {
            return engine.ValidateArtifactCompatibility(artifact, spec);
        },
        artifacts,
        seeds);
}

}  // namespace baseline
}  // namespace flowsql
