/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "config_parser.h"

#include <common/error_code.h>

#include <algorithm>
#include <optional>
#include <unordered_set>

#include <rapidjson/document.h>

namespace flowsql {
namespace baseline {

namespace {

int ParseObject(const char* config_json, rapidjson::Document* doc, std::string* err) {
    if (!config_json || !doc) {
        if (err) *err = "config_json must not be null";
        return error::BAD_REQUEST;
    }

    doc->Parse(config_json);
    if (doc->HasParseError() || !doc->IsObject()) {
        if (err) *err = "config_json must be a valid json object";
        return error::BAD_REQUEST;
    }
    return error::OK;
}

int RequireString(const rapidjson::Value& obj,
                  const char* name,
                  std::string* out,
                  std::string* err) {
    if (!out) return error::BAD_REQUEST;
    if (!obj.HasMember(name) || !obj[name].IsString()) {
        if (err) *err = std::string("missing string field: ") + name;
        return error::BAD_REQUEST;
    }

    *out = obj[name].GetString();
    if (out->empty()) {
        if (err) *err = std::string("field must not be empty: ") + name;
        return error::BAD_REQUEST;
    }
    return error::OK;
}

void OptionalString(const rapidjson::Value& obj,
                    const char* name,
                    std::string* out) {
    if (!out) return;
    if (obj.HasMember(name) && obj[name].IsString()) {
        *out = obj[name].GetString();
    }
}

int OptionalNonEmptyString(const rapidjson::Value& obj,
                           const char* name,
                           std::string* out,
                           std::string* err) {
    if (!out) return error::BAD_REQUEST;
    if (!obj.HasMember(name)) return error::OK;
    if (!obj[name].IsString()) {
        if (err) *err = std::string("field must be a string: ") + name;
        return error::BAD_REQUEST;
    }
    *out = obj[name].GetString();
    if (out->empty()) {
        if (err) *err = std::string("field must not be empty: ") + name;
        return error::BAD_REQUEST;
    }
    return error::OK;
}

int RequireInt64(const rapidjson::Value& obj,
                 const char* name,
                 int64_t* out,
                 std::string* err) {
    if (!out) return error::BAD_REQUEST;
    if (!obj.HasMember(name) || !obj[name].IsInt64()) {
        if (err) *err = std::string("missing int64 field: ") + name;
        return error::BAD_REQUEST;
    }
    *out = obj[name].GetInt64();
    return error::OK;
}

int OptionalBool(const rapidjson::Value& obj,
                 const char* name,
                 bool* out,
                 std::string* err) {
    if (!out) return error::BAD_REQUEST;
    if (!obj.HasMember(name)) return error::OK;
    if (!obj[name].IsBool()) {
        if (err) *err = std::string("field must be a bool: ") + name;
        return error::BAD_REQUEST;
    }
    *out = obj[name].GetBool();
    return error::OK;
}

int RequirePositiveInt64(const rapidjson::Value& obj,
                         const char* name,
                         int64_t* out,
                         std::string* err) {
    if (!out) return error::BAD_REQUEST;
    if (!obj.HasMember(name) || !obj[name].IsInt64()) {
        if (err) *err = std::string("missing int64 field: ") + name;
        return error::BAD_REQUEST;
    }
    const int64_t value = obj[name].GetInt64();
    if (value <= 0) {
        if (err) *err = std::string("field must be > 0: ") + name;
        return error::BAD_REQUEST;
    }
    *out = value;
    return error::OK;
}

int RequirePositiveInt32(const rapidjson::Value& obj,
                         const char* name,
                         int32_t* out,
                         std::string* err) {
    if (!out) return error::BAD_REQUEST;
    if (!obj.HasMember(name) || !obj[name].IsInt()) {
        if (err) *err = std::string("missing int field: ") + name;
        return error::BAD_REQUEST;
    }
    const int value = obj[name].GetInt();
    if (value <= 0) {
        if (err) *err = std::string("field must be > 0: ") + name;
        return error::BAD_REQUEST;
    }
    *out = value;
    return error::OK;
}

int RequireProbability(const rapidjson::Value& obj,
                       const char* name,
                       double* out,
                       std::string* err) {
    if (!out) return error::BAD_REQUEST;
    if (!obj.HasMember(name) || !obj[name].IsNumber()) {
        if (err) *err = std::string("missing number field: ") + name;
        return error::BAD_REQUEST;
    }
    const double value = obj[name].GetDouble();
    if (!(value > 0.0 && value <= 1.0)) {
        if (err) *err = std::string("field must be in (0,1]: ") + name;
        return error::BAD_REQUEST;
    }
    *out = value;
    return error::OK;
}

bool IsAllowedValue(const std::string& value,
                    std::initializer_list<const char*> allowed) {
    for (const char* item : allowed) {
        if (value == item) return true;
    }
    return false;
}

void ApplyDefaultTimezone(std::string* tz) {
    if (!tz || !tz->empty()) return;
    *tz = "Asia/Shanghai";
}

int ParseEventCalendarSpec(const rapidjson::Value& obj,
                           std::optional<EventCalendarSpec>* out,
                           std::string* err) {
    if (!out) return error::BAD_REQUEST;
    out->reset();

    if (!obj.HasMember("event_calendar_spec")) return error::OK;
    if (!obj["event_calendar_spec"].IsObject()) {
        if (err) *err = "event_calendar_spec must be an object";
        return error::BAD_REQUEST;
    }

    const auto& calendar = obj["event_calendar_spec"];
    EventCalendarSpec spec;
    int rc = RequireString(calendar, "calendar_id", &spec.calendar_id, err);
    if (rc != error::OK) return rc;
    rc = RequireString(calendar, "calendar_version", &spec.calendar_version, err);
    if (rc != error::OK) return rc;

    if (!calendar.HasMember("entries") || !calendar["entries"].IsArray()) {
        if (err) *err = "event_calendar_spec.entries must be an array";
        return error::BAD_REQUEST;
    }

    const auto& entries = calendar["entries"];
    spec.entries.reserve(entries.Size());
    for (rapidjson::SizeType i = 0; i < entries.Size(); ++i) {
        if (!entries[i].IsObject()) {
            if (err) *err = "event_calendar_spec.entries must contain objects only";
            return error::BAD_REQUEST;
        }

        EventCalendarEntry entry;
        if ((rc = RequireString(entries[i], "event_code", &entry.event_code, err)) != error::OK) {
            return rc;
        }
        if ((rc = RequireString(entries[i], "scope_type", &entry.scope_type, err)) != error::OK) {
            return rc;
        }
        if ((rc = RequireString(
                 entries[i], "alignment_mode", &entry.alignment_mode, err)) != error::OK) {
            return rc;
        }
        if (!IsAllowedValue(entry.scope_type, {"global", "feature", "key", "key_feature"})) {
            if (err) *err = "event_calendar_spec.scope_type is invalid";
            return error::BAD_REQUEST;
        }
        if (!IsAllowedValue(entry.alignment_mode, {"local_wall_clock", "absolute_utc"})) {
            if (err) *err = "event_calendar_spec.alignment_mode is invalid";
            return error::BAD_REQUEST;
        }
        if ((rc = RequireInt64(entries[i], "start_ts", &entry.start_ts, err)) != error::OK) {
            return rc;
        }
        if ((rc = RequireInt64(entries[i], "end_ts", &entry.end_ts, err)) != error::OK) {
            return rc;
        }
        if (entry.end_ts < entry.start_ts) {
            if (err) *err = "event_calendar_spec entry end_ts must be >= start_ts";
            return error::BAD_REQUEST;
        }
        if ((rc = OptionalBool(entries[i], "enabled", &entry.enabled, err)) != error::OK) {
            return rc;
        }
        if ((rc = OptionalNonEmptyString(entries[i], "feature", &entry.feature, err)) != error::OK) {
            return rc;
        }
        if ((rc = OptionalNonEmptyString(entries[i], "key", &entry.key, err)) != error::OK) {
            return rc;
        }
        if ((rc = OptionalNonEmptyString(entries[i], "tz", &entry.tz, err)) != error::OK) {
            return rc;
        }

        spec.entries.push_back(std::move(entry));
    }

    *out = std::move(spec);
    return error::OK;
}

int ParseBaselineSources(const rapidjson::Value& items,
                         const std::string& self_key,
                         const char* field_name,
                         BaselineSourceConfig* out,
                         std::string* err) {
    if (!out) return error::BAD_REQUEST;
    *out = BaselineSourceConfig{};
    if (!items.IsArray()) {
        if (err) *err = std::string(field_name) + " must be an array";
        return error::BAD_REQUEST;
    }
    if (items.Empty()) {
        if (err) *err = std::string(field_name) + " must not be empty";
        return error::BAD_REQUEST;
    }

    BaselineSourceConfig config;
    config.sources.reserve(items.Size());
    std::unordered_set<std::string> source_keys;
    for (rapidjson::SizeType i = 0; i < items.Size(); ++i) {
        if (!items[i].IsObject()) {
            if (err) *err = std::string(field_name) + " must contain objects only";
            return error::BAD_REQUEST;
        }

        BaselineSourceRef source_ref;
        const int rc = RequireString(items[i], "source_key", &source_ref.source_key, err);
        if (rc != error::OK) return rc;
        if (source_ref.source_key == self_key) {
            if (err) *err = "baseline source must not point to self";
            return error::BAD_REQUEST;
        }
        if (!source_keys.insert(source_ref.source_key).second) {
            if (err) *err = std::string(field_name) + " must not contain duplicate source_key";
            return error::BAD_REQUEST;
        }
        config.sources.push_back(std::move(source_ref));
    }

    *out = std::move(config);
    return error::OK;
}

int ParseBaselineSourceConfigs(const rapidjson::Value& obj,
                               std::vector<SeriesBaselineSourceConfig>* out,
                               std::string* err) {
    if (!out) return error::BAD_REQUEST;
    out->clear();

    if (obj.HasMember("baseline_source_config")) {
        if (err) {
            *err = "baseline_source_config is task-global and not supported; use baseline_source_configs";
        }
        return error::BAD_REQUEST;
    }
    if (obj.HasMember("series_overrides")) {
        if (err) *err = "series_overrides is no longer supported; use baseline_source_configs";
        return error::BAD_REQUEST;
    }
    if (!obj.HasMember("baseline_source_configs")) return error::OK;
    if (!obj["baseline_source_configs"].IsArray()) {
        if (err) *err = "baseline_source_configs must be an array";
        return error::BAD_REQUEST;
    }

    const auto& entries = obj["baseline_source_configs"];
    if (entries.Empty()) {
        if (err) *err = "baseline_source_configs must not be empty";
        return error::BAD_REQUEST;
    }

    std::unordered_set<std::string> keys;
    out->reserve(entries.Size());
    for (rapidjson::SizeType i = 0; i < entries.Size(); ++i) {
        if (!entries[i].IsObject()) {
            if (err) *err = "baseline_source_configs must contain objects only";
            return error::BAD_REQUEST;
        }

        SeriesBaselineSourceConfig entry;
        int rc = RequireString(entries[i], "key", &entry.key, err);
        if (rc != error::OK) return rc;
        if (!keys.insert(entry.key).second) {
            if (err) *err = "baseline_source_configs must not contain duplicate key";
            return error::BAD_REQUEST;
        }
        if (!entries[i].HasMember("baseline_sources")) {
            if (err) *err = "missing field: baseline_sources";
            return error::BAD_REQUEST;
        }
        rc = ParseBaselineSources(
            entries[i]["baseline_sources"],
            entry.key,
            "baseline_sources",
            &entry.config,
            err);
        if (rc != error::OK) return rc;

        out->push_back(std::move(entry));
    }

    return error::OK;
}

int ParseScalarTaskInternal(const char* config_json,
                            BaselineTaskSpec* out,
                            std::string* err) {
    if (!out) return error::BAD_REQUEST;

    rapidjson::Document doc;
    int rc = ParseObject(config_json, &doc, err);
    if (rc != error::OK) return rc;

    BaselineTaskSpec spec;
    if ((rc = RequireString(doc, "key", &spec.key, err)) != error::OK) return rc;
    if ((rc = RequireString(doc, "feature", &spec.feature, err)) != error::OK) return rc;
    if ((rc = RequireString(doc, "feature_type", &spec.feature_type, err)) != error::OK) return rc;
    if ((rc = RequireString(doc, "feature_profile", &spec.feature_profile, err)) != error::OK) return rc;
    if ((rc = RequirePositiveInt64(doc, "delta", &spec.delta, err)) != error::OK) return rc;
    if ((rc = OptionalNonEmptyString(doc, "tz", &spec.tz, err)) != error::OK) return rc;
    ApplyDefaultTimezone(&spec.tz);
    if ((rc = ParseBaselineSourceConfigs(doc, &spec.baseline_source_configs, err)) != error::OK) return rc;
    if ((rc = ParseEventCalendarSpec(doc, &spec.event_calendar_spec, err)) != error::OK) return rc;

    OptionalString(doc, "name", &spec.name);
    if (spec.name.empty()) spec.name = spec.feature;
    spec.config_json = config_json;

    *out = std::move(spec);
    return error::OK;
}

int ParseMetricList(const rapidjson::Value& obj,
                    std::vector<std::string>* out,
                    std::string* err) {
    if (!out) return error::BAD_REQUEST;
    if (!obj.HasMember("metrics") || !obj["metrics"].IsArray()) {
        if (err) *err = "missing string array field: metrics";
        return error::BAD_REQUEST;
    }

    const auto& metrics = obj["metrics"];
    if (metrics.Empty()) {
        if (err) *err = "metrics must not be empty";
        return error::BAD_REQUEST;
    }

    out->clear();
    out->reserve(metrics.Size());
    std::unordered_set<std::string> uniq_metrics;
    for (rapidjson::SizeType i = 0; i < metrics.Size(); ++i) {
        if (!metrics[i].IsString()) {
            if (err) *err = "metrics must contain strings only";
            return error::BAD_REQUEST;
        }
        std::string metric = metrics[i].GetString();
        if (metric.empty()) {
            if (err) *err = "metrics must not contain empty item";
            return error::BAD_REQUEST;
        }
        if (!uniq_metrics.insert(metric).second) {
            if (err) *err = "metrics must not contain duplicate item";
            return error::BAD_REQUEST;
        }
        out->push_back(std::move(metric));
    }
    return error::OK;
}

int ParseOtherGroupIdxs(const rapidjson::Value& obj,
                        std::vector<uint32_t>* out,
                        std::string* err) {
    if (!out) return error::BAD_REQUEST;
    out->clear();
    std::unordered_set<uint32_t> uniq_group_idxs;

    if (obj.HasMember("other_group_idx")) {
        if (!obj["other_group_idx"].IsUint()) {
            if (err) *err = "other_group_idx must be an uint";
            return error::BAD_REQUEST;
        }
        const uint32_t group_idx = obj["other_group_idx"].GetUint();
        if (!uniq_group_idxs.insert(group_idx).second) {
            if (err) *err = "other_group_idx must not duplicate with other_group_idxs";
            return error::BAD_REQUEST;
        }
        out->push_back(group_idx);
    }

    if (obj.HasMember("other_group_idxs")) {
        if (!obj["other_group_idxs"].IsArray()) {
            if (err) *err = "other_group_idxs must be an array";
            return error::BAD_REQUEST;
        }
        const auto& other_group_idxs = obj["other_group_idxs"];
        for (rapidjson::SizeType i = 0; i < other_group_idxs.Size(); ++i) {
            if (!other_group_idxs[i].IsUint()) {
                if (err) *err = "other_group_idxs must contain uint only";
                return error::BAD_REQUEST;
            }
            const uint32_t group_idx = other_group_idxs[i].GetUint();
            if (!uniq_group_idxs.insert(group_idx).second) {
                if (err) *err = "other_group_idxs must not contain duplicate item";
                return error::BAD_REQUEST;
            }
            out->push_back(group_idx);
        }
    }

    std::sort(out->begin(), out->end());
    return error::OK;
}

int ParseSupportPolicy(const rapidjson::Value& obj,
                       RelationSupportPolicySpec* out,
                       std::string* err) {
    if (!out) return error::BAD_REQUEST;
    if (!obj.HasMember("support_policy") || !obj["support_policy"].IsObject()) {
        if (err) *err = "missing object field: support_policy";
        return error::BAD_REQUEST;
    }

    int rc = RequirePositiveInt32(
        obj["support_policy"], "k_support", &out->k_support, err);
    if (rc != error::OK) return rc;
    rc = RequireProbability(
        obj["support_policy"], "min_hist_share", &out->min_hist_share, err);
    if (rc != error::OK) return rc;
    return RequireProbability(
        obj["support_policy"], "min_active_ratio", &out->min_active_ratio, err);
}

int ParseSummaryPolicy(const rapidjson::Value& obj,
                       RelationSummaryPolicySpec* out,
                       std::string* err) {
    if (!out) return error::BAD_REQUEST;
    if (!obj.HasMember("summary_policy") || !obj["summary_policy"].IsObject()) {
        if (err) *err = "missing object field: summary_policy";
        return error::BAD_REQUEST;
    }

    int rc = RequirePositiveInt32(obj["summary_policy"], "k_head", &out->k_head, err);
    if (rc != error::OK) return rc;
    return RequirePositiveInt32(obj["summary_policy"], "k_stable", &out->k_stable, err);
}

}  // namespace

int ConfigParser::ParseValueTask(const char* config_json,
                                 BaselineTaskSpec* out,
                                 std::string* err) {
    int rc = ParseScalarTaskInternal(config_json, out, err);
    if (rc != error::OK) return rc;

    if (out->feature_type != "t1a" && out->feature_type != "t1b") {
        if (err) *err = "feature_type for value task must be t1a or t1b";
        return error::BAD_REQUEST;
    }
    if (out->feature_type == "t1b" &&
        out->feature_profile != "cont_core" &&
        out->feature_profile != "cont_tail") {
        if (err) *err = "t1b feature_profile must be cont_core or cont_tail";
        return error::BAD_REQUEST;
    }
    return error::OK;
}

int ConfigParser::ParseRatioTask(const char* config_json,
                                 BaselineTaskSpec* out,
                                 std::string* err) {
    int rc = ParseScalarTaskInternal(config_json, out, err);
    if (rc != error::OK) return rc;

    if (out->feature_type != "t2" && out->feature_type != "ratio") {
        if (err) *err = "feature_type for ratio task must be t2 or ratio";
        return error::BAD_REQUEST;
    }
    if (out->feature_profile != "rate_core" &&
        out->feature_profile != "ratio_bursty") {
        if (err) *err = "t2 feature_profile must be rate_core or ratio_bursty";
        return error::BAD_REQUEST;
    }
    return error::OK;
}

int ConfigParser::ParseRelationTask(const char* config_json,
                                    RelationTaskCreateSpec* out,
                                    std::string* err) {
    if (!out) return error::BAD_REQUEST;

    rapidjson::Document doc;
    int rc = ParseObject(config_json, &doc, err);
    if (rc != error::OK) return rc;

    RelationTaskCreateSpec create_spec;
    RelationTaskSpec& spec = create_spec.task_spec;
    if ((rc = RequireString(doc, "feature_base", &spec.feature_base, err)) != error::OK) return rc;
    if ((rc = RequireString(doc, "group_space_id", &spec.group_space_id, err)) != error::OK) return rc;
    if ((rc = RequireString(doc, "metric_set_id", &spec.metric_set_id, err)) != error::OK) return rc;
    if ((rc = RequireString(doc, "encode_type", &spec.encode_type, err)) != error::OK) return rc;
    if ((rc = ParseMetricList(doc, &spec.metrics, err)) != error::OK) return rc;
    if ((rc = ParseOtherGroupIdxs(doc, &spec.other_group_idxs, err)) != error::OK) return rc;
    if ((rc = ParseSupportPolicy(doc, &spec.support_policy, err)) != error::OK) return rc;
    if ((rc = ParseSummaryPolicy(doc, &spec.summary_policy, err)) != error::OK) return rc;
    if ((rc = RequirePositiveInt64(doc, "delta", &create_spec.clock_spec.delta, err)) != error::OK) return rc;
    if ((rc = OptionalNonEmptyString(doc, "tz", &create_spec.clock_spec.tz, err)) != error::OK) return rc;
    ApplyDefaultTimezone(&create_spec.clock_spec.tz);
    if ((rc = ParseEventCalendarSpec(doc, &create_spec.event_calendar_spec, err)) != error::OK) return rc;

    OptionalString(doc, "name", &spec.name);
    if (doc.HasMember("group_space_version")) {
        std::string group_space_version;
        if ((rc = OptionalNonEmptyString(doc, "group_space_version", &group_space_version, err)) != error::OK) {
            return rc;
        }
        if (!group_space_version.empty()) spec.group_space_version = group_space_version;
    }
    if (!IsAllowedValue(spec.encode_type, {"exact_sparse", "topk_other"})) {
        if (err) *err = "relation encode_type must be exact_sparse or topk_other";
        return error::BAD_REQUEST;
    }
    if (!spec.other_group_idxs.empty() && spec.encode_type != "topk_other") {
        if (err) *err = "other_group_idx(s) requires relation encode_type=topk_other";
        return error::BAD_REQUEST;
    }
    if (spec.support_policy.k_support < spec.summary_policy.k_stable) {
        if (err) *err = "support_policy.k_support must be >= summary_policy.k_stable";
        return error::BAD_REQUEST;
    }
    if (spec.name.empty()) spec.name = spec.feature_base;
    spec.config_json = config_json;

    *out = std::move(create_spec);
    return error::OK;
}

}  // namespace baseline
}  // namespace flowsql
