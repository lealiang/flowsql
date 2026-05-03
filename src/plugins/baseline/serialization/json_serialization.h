/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_SERIALIZATION_JSON_SERIALIZATION_H_
#define _FLOWSQL_PLUGINS_BASELINE_SERIALIZATION_JSON_SERIALIZATION_H_

#include <framework/interfaces/ibaseline_types.h>
#include <rapidjson/document.h>

#include <string>

#include "plugins/baseline/bootstrap/bootstrap_types.h"
#include "plugins/baseline/model/task_spec.h"

namespace flowsql {
namespace baseline {

inline const char* ArtifactKindName(BootstrapArtifactKind kind) {
    switch (kind) {
        case BootstrapArtifactKind::kValue:
            return "value";
        case BootstrapArtifactKind::kRatio:
            return "ratio";
        case BootstrapArtifactKind::kRelation:
            return "relation";
        case BootstrapArtifactKind::kNone:
            break;
    }
    return "none";
}

inline BootstrapArtifactKind ParseArtifactKind(const std::string& name) {
    if (name == "value") return BootstrapArtifactKind::kValue;
    if (name == "ratio") return BootstrapArtifactKind::kRatio;
    if (name == "relation") return BootstrapArtifactKind::kRelation;
    return BootstrapArtifactKind::kNone;
}

inline const char* TaskKindName(BaselineTaskKind kind) {
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

inline bool ParseTaskKindName(const std::string& name, BaselineTaskKind* out) {
    if (!out) return false;
    if (name == "value") {
        *out = BaselineTaskKind::kValue;
        return true;
    }
    if (name == "ratio") {
        *out = BaselineTaskKind::kRatio;
        return true;
    }
    if (name == "relation") {
        *out = BaselineTaskKind::kRelation;
        return true;
    }
    return false;
}

template <typename Writer>
inline void WriteStringField(Writer* writer,
                             const char* name,
                             const std::string& value) {
    writer->Key(name);
    writer->String(value.c_str());
}

template <typename Writer>
inline void WriteTaskIdentity(Writer* writer, const BootstrapTaskIdentity& identity) {
    writer->Key("task_identity");
    writer->StartObject();
    WriteStringField(writer, "task_id", identity.task_id);
    WriteStringField(writer, "task_kind", identity.task_kind);
    WriteStringField(writer, "feature_type", identity.feature_type);
    WriteStringField(writer, "feature_id", identity.feature_id);
    WriteStringField(writer, "profile", identity.profile);
    writer->EndObject();
}

template <typename Writer>
inline void WriteTaskIdentity(Writer* writer, const BaselineTaskSpec& spec) {
    writer->Key("task_identity");
    writer->StartObject();
    WriteStringField(writer, "task_id", spec.task_id);
    WriteStringField(writer, "task_kind", spec.task_kind);
    WriteStringField(writer, "feature_id", spec.feature_id);
    WriteStringField(writer, "feature_type", spec.feature_type);
    WriteStringField(writer, "profile", spec.profile);
    writer->EndObject();
}

template <typename Writer>
inline void WriteClockSpec(Writer* writer, const BootstrapClockSpec& clock) {
    writer->Key("clock_spec");
    writer->StartObject();
    writer->Key("bucket_seconds");
    writer->Int64(clock.bucket_seconds);
    WriteStringField(writer, "timezone", clock.timezone);
    writer->EndObject();
}

template <typename Writer>
inline void WriteCalendarRef(Writer* writer, const BootstrapCalendarRef& calendar) {
    writer->Key("calendar_ref");
    writer->StartObject();
    WriteStringField(writer, "calendar_id", calendar.calendar_id);
    WriteStringField(writer, "calendar_version", calendar.calendar_version);
    writer->EndObject();
}

inline bool ReadString(const rapidjson::Value& obj, const char* name, std::string* out) {
    if (!out || !obj.HasMember(name) || !obj[name].IsString()) return false;
    *out = obj[name].GetString();
    return true;
}

inline bool ReadTaskIdentity(const rapidjson::Value& obj, BootstrapTaskIdentity* out) {
    if (!out || !obj.HasMember("task_identity") || !obj["task_identity"].IsObject()) {
        return false;
    }
    const auto& identity = obj["task_identity"];
    return ReadString(identity, "task_id", &out->task_id) &&
           ReadString(identity, "task_kind", &out->task_kind) &&
           ReadString(identity, "feature_type", &out->feature_type) &&
           ReadString(identity, "feature_id", &out->feature_id) &&
           ReadString(identity, "profile", &out->profile);
}

inline bool ReadClockSpec(const rapidjson::Value& obj, BootstrapClockSpec* out) {
    if (!out || !obj.HasMember("clock_spec") || !obj["clock_spec"].IsObject()) {
        return false;
    }
    const auto& clock = obj["clock_spec"];
    if (!clock.HasMember("bucket_seconds") || !clock["bucket_seconds"].IsInt64()) {
        return false;
    }
    out->bucket_seconds = clock["bucket_seconds"].GetInt64();
    return ReadString(clock, "timezone", &out->timezone);
}

inline bool ReadCalendarRef(const rapidjson::Value& obj, BootstrapCalendarRef* out) {
    if (!out || !obj.HasMember("calendar_ref") || !obj["calendar_ref"].IsObject()) {
        return false;
    }
    const auto& calendar = obj["calendar_ref"];
    return ReadString(calendar, "calendar_id", &out->calendar_id) &&
           ReadString(calendar, "calendar_version", &out->calendar_version);
}

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_SERIALIZATION_JSON_SERIALIZATION_H_
