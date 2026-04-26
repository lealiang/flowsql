/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/relation/relation_router.h"

#include <common/error_code.h>

#include <optional>
#include <string>
#include <unordered_set>

#include <rapidjson/document.h>

namespace flowsql {
namespace baseline {

namespace {

std::string MakeFeatureId(const RelationTaskSpec& spec,
                          const std::string& metric_name,
                          const char* suffix) {
    return spec.feature_base + "_" + metric_name + "_" + suffix;
}

std::string CopyStringRef(const BaselineStringRef& key) {
    if (!key.data || key.size == 0) return "";
    return std::string(key.data, key.size);
}

bool ParseResolvedSourceConfig(const std::string& config_json,
                               const std::string& self_key,
                               std::optional<BaselineSourceConfig>* out_config) {
    if (!out_config) return false;
    out_config->reset();
    if (config_json.empty()) return true;

    rapidjson::Document doc;
    doc.Parse(config_json.c_str());
    if (doc.HasParseError()) return false;

    const rapidjson::Value* source_array = nullptr;
    if (doc.IsArray()) {
        source_array = &doc;
    } else if (doc.IsObject() && doc.HasMember("baseline_sources") &&
               doc["baseline_sources"].IsArray()) {
        source_array = &doc["baseline_sources"];
    } else {
        return false;
    }

    if (!source_array || source_array->Empty()) return true;

    BaselineSourceConfig config;
    config.sources.reserve(source_array->Size());
    std::unordered_set<std::string> seen_sources;
    for (rapidjson::SizeType i = 0; i < source_array->Size(); ++i) {
        if (!(*source_array)[i].IsObject() || !(*source_array)[i].HasMember("source_key") ||
            !(*source_array)[i]["source_key"].IsString()) {
            return false;
        }

        BaselineSourceRef ref;
        ref.source_key = (*source_array)[i]["source_key"].GetString();
        if (ref.source_key.empty() || ref.source_key == self_key ||
            !seen_sources.insert(ref.source_key).second) {
            return false;
        }
        config.sources.push_back(std::move(ref));
    }

    if (!config.empty()) *out_config = std::move(config);
    return true;
}

bool EventEntryMatchesRoutedFeature(const EventCalendarEntry& entry,
                                    const std::string& key,
                                    const std::string& feature) {
    if (!entry.enabled) return false;

    const bool feature_match = entry.feature.empty() || entry.feature == feature;
    const bool key_match = entry.key.empty() || entry.key == key;

    if (entry.scope_type == "global") return true;
    if (entry.scope_type == "feature") return feature_match;
    if (entry.scope_type == "key") return key_match;
    if (entry.scope_type == "key_feature") return feature_match && key_match;
    return false;
}

std::optional<EventCalendarSpec> CropEventCalendarSpec(
    const EventCalendarSpec* event_calendar_spec,
    const std::string& key,
    const std::string& feature) {
    if (!event_calendar_spec) return std::nullopt;

    EventCalendarSpec routed_calendar;
    routed_calendar.calendar_id = event_calendar_spec->calendar_id;
    routed_calendar.calendar_version = event_calendar_spec->calendar_version;
    for (const auto& entry : event_calendar_spec->entries) {
        if (EventEntryMatchesRoutedFeature(entry, key, feature)) {
            routed_calendar.entries.push_back(entry);
        }
    }

    if (routed_calendar.entries.empty()) return std::nullopt;
    return routed_calendar;
}

std::optional<BaselineSourceConfig> ResolveRoutedSourceConfig(
    IBaselineSourceResolver* source_resolver,
    const BaselineStringRef& key,
    const std::string& feature) {
    if (!source_resolver) return std::nullopt;

    std::string config_json;
    const BaselineStringRef feature_ref{feature.c_str(), static_cast<uint32_t>(feature.size())};
    if (source_resolver->ResolveBaselineSource(key, feature_ref, &config_json) != error::OK) {
        return std::nullopt;
    }

    std::optional<BaselineSourceConfig> parsed_config;
    if (!ParseResolvedSourceConfig(config_json, CopyStringRef(key), &parsed_config)) {
        return std::nullopt;
    }
    return parsed_config;
}

void AppendRoutedFeatureSpec(const RelationTaskSpec& spec,
                             const RelationTaskClockSpec& clock_spec,
                             const BaselineStringRef& key,
                             const EventCalendarSpec* event_calendar_spec,
                             IBaselineSourceResolver* source_resolver,
                             const std::string& metric_name,
                             const std::string& feature,
                             RelationRoutedDetectorKind detector_kind,
                             RelationSummaryKind summary_kind,
                             const std::string& feature_type,
                             const std::string& feature_profile,
                             const std::optional<std::string>& transform_kind,
                             int stable_index,
                             int32_t* local_slot,
                             std::vector<RelationRoutedFeatureSpec>* out_specs) {
    if (!local_slot || !out_specs) return;

    RelationRoutedFeatureSpec routed_spec;
    routed_spec.local_slot = (*local_slot)++;
    routed_spec.metric_name = metric_name;
    routed_spec.feature = feature;
    routed_spec.routed_feature_id = feature;
    routed_spec.detector_kind = detector_kind;
    routed_spec.summary_kind = summary_kind;
    routed_spec.feature_type = feature_type;
    routed_spec.feature_profile = feature_profile;
    routed_spec.transform_kind = transform_kind;
    routed_spec.delta = clock_spec.delta;
    routed_spec.tz = clock_spec.tz;
    routed_spec.baseline_source_config =
        ResolveRoutedSourceConfig(source_resolver, key, feature);
    routed_spec.event_calendar_spec =
        CropEventCalendarSpec(event_calendar_spec, CopyStringRef(key), feature);
    routed_spec.stable_index = stable_index;
    out_specs->push_back(std::move(routed_spec));
}

}  // namespace

void RelationRouter::BuildRoutedFeatureSpecs(
    const RelationTaskSpec& spec,
    const RelationServiceBasis& basis,
    const RelationTaskClockSpec& clock_spec,
    const BaselineStringRef& key,
    const EventCalendarSpec* event_calendar_spec,
    IBaselineSourceResolver* source_resolver,
    std::vector<RelationRoutedFeatureSpec>* out_specs) {
    if (!out_specs) return;
    out_specs->clear();

    int32_t local_slot = 0;
    const int32_t stable_size = static_cast<int32_t>(basis.stable_head.size());
    for (const auto& metric_name : spec.metrics) {
        AppendRoutedFeatureSpec(spec,
                                clock_spec,
                                key,
                                event_calendar_spec,
                                source_resolver,
                                metric_name,
                                MakeFeatureId(spec, metric_name, "entropy_shannon"),
                                RelationRoutedDetectorKind::kValue,
                                RelationSummaryKind::kEntropyShannon,
                                "value_basic",
                                "default",
                                std::optional<std::string>("identity"),
                                -1,
                                &local_slot,
                                out_specs);
        AppendRoutedFeatureSpec(spec,
                                clock_spec,
                                key,
                                event_calendar_spec,
                                source_resolver,
                                metric_name,
                                MakeFeatureId(spec, metric_name, "top1_share"),
                                RelationRoutedDetectorKind::kRatio,
                                RelationSummaryKind::kTop1Share,
                                "ratio",
                                "rate_core",
                                std::nullopt,
                                -1,
                                &local_slot,
                                out_specs);
        AppendRoutedFeatureSpec(spec,
                                clock_spec,
                                key,
                                event_calendar_spec,
                                source_resolver,
                                metric_name,
                                MakeFeatureId(spec, metric_name, "headk_share"),
                                RelationRoutedDetectorKind::kRatio,
                                RelationSummaryKind::kHeadKShare,
                                "ratio",
                                "rate_core",
                                std::nullopt,
                                -1,
                                &local_slot,
                                out_specs);
        AppendRoutedFeatureSpec(spec,
                                clock_spec,
                                key,
                                event_calendar_spec,
                                source_resolver,
                                metric_name,
                                MakeFeatureId(spec, metric_name, "out_of_support_share"),
                                RelationRoutedDetectorKind::kRatio,
                                RelationSummaryKind::kOutOfSupportShare,
                                "ratio",
                                "ratio_bursty",
                                std::nullopt,
                                -1,
                                &local_slot,
                                out_specs);
        AppendRoutedFeatureSpec(spec,
                                clock_spec,
                                key,
                                event_calendar_spec,
                                source_resolver,
                                metric_name,
                                MakeFeatureId(spec, metric_name, "distinct_group_count"),
                                RelationRoutedDetectorKind::kValue,
                                RelationSummaryKind::kDistinctGroupCount,
                                "value_basic",
                                "default",
                                std::optional<std::string>("log1p"),
                                -1,
                                &local_slot,
                                out_specs);
        for (int32_t i = 0; i < stable_size; ++i) {
            AppendRoutedFeatureSpec(
                spec,
                clock_spec,
                key,
                event_calendar_spec,
                source_resolver,
                metric_name,
                MakeFeatureId(spec,
                              metric_name,
                              ("stable_g" + std::to_string(i + 1) + "_share").c_str()),
                RelationRoutedDetectorKind::kRatio,
                RelationSummaryKind::kStableGShare,
                "ratio",
                "rate_core",
                std::nullopt,
                i,
                &local_slot,
                out_specs);
        }
        if (stable_size >= 1) {
            AppendRoutedFeatureSpec(spec,
                                    clock_spec,
                                    key,
                                    event_calendar_spec,
                                    source_resolver,
                                    metric_name,
                                    MakeFeatureId(spec, metric_name, "stable_head_coverage"),
                                    RelationRoutedDetectorKind::kRatio,
                                    RelationSummaryKind::kStableHeadCoverage,
                                    "ratio",
                                    "rate_core",
                                    std::nullopt,
                                    -1,
                                    &local_slot,
                                    out_specs);
        }
        if (stable_size >= 2) {
            AppendRoutedFeatureSpec(spec,
                                    clock_spec,
                                    key,
                                    event_calendar_spec,
                                    source_resolver,
                                    metric_name,
                                    MakeFeatureId(spec, metric_name, "stable_head_mix_drift"),
                                    RelationRoutedDetectorKind::kValue,
                                    RelationSummaryKind::kStableHeadMixDrift,
                                    "value_basic",
                                    "default",
                                    std::optional<std::string>("identity"),
                                    -1,
                                    &local_slot,
                                    out_specs);
        }
    }
}

bool RelationRouter::BuildValueObservation(const RelationRoutedFeatureSpec& feature_spec,
                                           const BaselineStringRef& key,
                                           int64_t bucket_id,
                                           const RelationMetricSummary& summary,
                                           ValueObservation* out_observation) {
    if (!out_observation || feature_spec.detector_kind != RelationRoutedDetectorKind::kValue ||
        !summary.valid) {
        return false;
    }

    double value = 0.0;
    switch (feature_spec.summary_kind) {
        case RelationSummaryKind::kEntropyShannon:
            value = summary.entropy_shannon;
            break;
        case RelationSummaryKind::kDistinctGroupCount:
            if (!summary.has_distinct_group_count) return false;
            value = summary.distinct_group_count;
            break;
        case RelationSummaryKind::kStableHeadMixDrift:
            if (!summary.has_stable_headk_mix_drift) return false;
            value = summary.stable_headk_mix_drift;
            break;
        default:
            return false;
    }

    *out_observation = ValueObservation{key, bucket_id, value, 0};
    return true;
}

bool RelationRouter::BuildRatioObservation(const RelationRoutedFeatureSpec& feature_spec,
                                           const BaselineStringRef& key,
                                           int64_t bucket_id,
                                           const RelationMetricSummary& summary,
                                           RatioObservation* out_observation) {
    if (!out_observation || feature_spec.detector_kind != RelationRoutedDetectorKind::kRatio ||
        !summary.valid || summary.total <= 0.0) {
        return false;
    }

    double numerator = 0.0;
    switch (feature_spec.summary_kind) {
        case RelationSummaryKind::kTop1Share:
            numerator = summary.top1_share * summary.total;
            break;
        case RelationSummaryKind::kHeadKShare:
            numerator = summary.headk_share * summary.total;
            break;
        case RelationSummaryKind::kOutOfSupportShare:
            numerator = summary.out_of_support_share * summary.total;
            break;
        case RelationSummaryKind::kStableGShare:
            if (feature_spec.stable_index < 0 ||
                static_cast<size_t>(feature_spec.stable_index) >=
                    summary.stable_g_shares.size()) {
                return false;
            }
            numerator =
                summary.stable_g_shares[static_cast<size_t>(feature_spec.stable_index)] *
                summary.total;
            break;
        case RelationSummaryKind::kStableHeadCoverage:
            if (summary.stable_g_shares.empty()) return false;
            numerator = summary.stable_headk_coverage * summary.total;
            break;
        default:
            return false;
    }

    *out_observation = RatioObservation{key, bucket_id, numerator, summary.total};
    return true;
}

}  // namespace baseline
}  // namespace flowsql
