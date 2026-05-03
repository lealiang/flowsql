/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "routed_summary.h"

#include <string>
#include <string_view>
#include <utility>

namespace flowsql {
namespace baseline {

namespace {

const char* TaskKindName(BaselineTaskKind kind) {
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

std::string RoutedFeatureId(const RelationTaskCreateSpec& spec,
                            const std::string& metric_name,
                            const std::string& summary_name) {
    const std::string feature_base = spec.task_spec.feature_base.empty()
                                         ? spec.task_spec.feature_id
                                         : spec.task_spec.feature_base;
    return feature_base + "." + metric_name + "." + summary_name;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

BootstrapArtifactKind ArtifactKindForTask(BaselineTaskKind task_kind) {
    if (task_kind == BaselineTaskKind::kValue) return BootstrapArtifactKind::kValue;
    if (task_kind == BaselineTaskKind::kRatio) return BootstrapArtifactKind::kRatio;
    return BootstrapArtifactKind::kNone;
}

BootstrapTaskIdentity TaskIdentityFromSpec(const BaselineTaskSpec& spec) {
    BootstrapTaskIdentity identity;
    identity.task_id = spec.task_id;
    identity.task_kind = spec.task_kind;
    identity.feature_type = spec.feature_type;
    identity.feature_id = spec.feature_id;
    identity.profile = spec.profile;
    return identity;
}

bool EmptyTaskIdentity(const BootstrapTaskIdentity& identity) {
    return identity.task_id.empty() && identity.task_kind.empty() &&
           identity.feature_type.empty() && identity.feature_id.empty() &&
           identity.profile.empty();
}

BootstrapClockSpec ClockSpecFromTask(const BaselineTaskSpec& spec) {
    BootstrapClockSpec clock;
    clock.bucket_seconds = spec.clock_spec.bucket_seconds;
    clock.timezone = spec.clock_spec.timezone.empty() ? spec.tz : spec.clock_spec.timezone;
    if (clock.timezone.empty()) clock.timezone = "UTC";
    return clock;
}

BootstrapCalendarRef CalendarRefFromTask(const BaselineTaskSpec& spec) {
    BootstrapCalendarRef calendar;
    calendar.calendar_id = spec.calendar_ref.calendar_id;
    calendar.calendar_version = spec.calendar_ref.calendar_version;
    return calendar;
}

bool EmptyClockSpec(const BootstrapClockSpec& clock) {
    return clock.bucket_seconds == 0 && clock.timezone.empty();
}

}  // namespace

const char* RelationSummaryFeatureType(BaselineTaskKind task_kind) {
    return task_kind == BaselineTaskKind::kValue ? "value_basic" : TaskKindName(task_kind);
}

bool IsBasisScopedRelationSummary(std::string_view summary_name) {
    return summary_name == "out_of_support_share" ||
           summary_name == "stable_headk_coverage" ||
           summary_name == "stable_headk_mix_drift" ||
           StartsWith(summary_name, "stable_g_share_");
}

RelationRoutedSummaryIdentity MakeRelationRoutedSummaryIdentity(
    std::string_view source_series_key,
    std::string_view metric_name,
    std::string_view summary_name,
    BaselineTaskKind task_kind,
    uint64_t basis_version) {
    RelationRoutedSummaryIdentity identity;
    identity.source_series_key = std::string(source_series_key);
    identity.metric_name = std::string(metric_name);
    identity.summary_name = std::string(summary_name);
    identity.feature_type = RelationSummaryFeatureType(task_kind);
    identity.basis_scoped = IsBasisScopedRelationSummary(summary_name);
    identity.basis_version = identity.basis_scoped ? basis_version : 0;
    identity.routed_series_key = identity.source_series_key + "::" +
                                 identity.metric_name + "::" +
                                 identity.summary_name + "::" +
                                 identity.feature_type;
    if (identity.basis_scoped) {
        identity.routed_series_key += "::basis:" + std::to_string(identity.basis_version);
    }
    return identity;
}

BaselineTaskSpec MakeRoutedSummaryTaskSpec(
    const RelationTaskCreateSpec& spec,
    const std::string& metric_name,
    const std::string& summary_name,
    BaselineTaskKind task_kind) {
    BaselineTaskSpec task_spec;
    task_spec.task_id = spec.task_spec.task_id + "::" + metric_name + "::" + summary_name;
    task_spec.name = task_spec.task_id;
    task_spec.task_kind = TaskKindName(task_kind);
    task_spec.feature_id = RoutedFeatureId(spec, metric_name, summary_name);
    task_spec.feature = task_spec.feature_id;
    task_spec.feature_type = RelationSummaryFeatureType(task_kind);
    task_spec.profile = task_kind == BaselineTaskKind::kRatio ? "rate_core" : "default";
    task_spec.clock_spec.bucket_seconds = spec.clock_spec.delta;
    task_spec.clock_spec.timezone = spec.clock_spec.tz;
    task_spec.calendar_ref = spec.task_spec.calendar_ref;
    task_spec.delta = spec.clock_spec.delta;
    task_spec.tz = spec.clock_spec.tz;
    return task_spec;
}

BaselineStatus MaterializeRelationRoutedBootstrapSeed(
    const RelationTaskCreateSpec& spec,
    std::string_view source_series_key,
    const RelationRoutedBootstrapSeed& routed_seed,
    uint64_t fallback_basis_version,
    RelationRoutedBootstrapSeedMaterialization* out) {
    if (!out || source_series_key.empty() || routed_seed.metric_name.empty() ||
        routed_seed.summary_name.empty()) {
        return BaselineStatus::kInvalidArgument;
    }
    if (routed_seed.task_kind != BaselineTaskKind::kValue &&
        routed_seed.task_kind != BaselineTaskKind::kRatio) {
        return BaselineStatus::kInvalidArgument;
    }

    const bool fixed_basis_scoped = IsBasisScopedRelationSummary(routed_seed.summary_name);
    uint64_t basis_version = 0;
    if (fixed_basis_scoped) {
        basis_version = routed_seed.basis_version > 0 ? routed_seed.basis_version
                                                      : fallback_basis_version;
        if (basis_version == 0) return BaselineStatus::kInvalidArgument;
    }

    RelationRoutedBootstrapSeedMaterialization materialized;
    materialized.task_spec = MakeRoutedSummaryTaskSpec(
        spec, routed_seed.metric_name, routed_seed.summary_name, routed_seed.task_kind);
    const RelationRoutedSummaryIdentity identity = MakeRelationRoutedSummaryIdentity(
        source_series_key,
        routed_seed.metric_name,
        routed_seed.summary_name,
        routed_seed.task_kind,
        basis_version);
    materialized.routed_series_key = identity.routed_series_key;

    materialized.seed.artifact_kind = ArtifactKindForTask(routed_seed.task_kind);
    materialized.seed.seed_status = routed_seed.seed_status;
    materialized.seed.series_key = materialized.routed_series_key;
    materialized.seed.task_identity =
        EmptyTaskIdentity(routed_seed.task_identity)
            ? TaskIdentityFromSpec(materialized.task_spec)
            : routed_seed.task_identity;
    materialized.seed.clock_spec =
        EmptyClockSpec(routed_seed.clock_spec)
            ? ClockSpecFromTask(materialized.task_spec)
            : routed_seed.clock_spec;
    materialized.seed.calendar_ref = routed_seed.calendar_ref.calendar_id.empty() &&
                                             routed_seed.calendar_ref.calendar_version.empty()
                                         ? CalendarRefFromTask(materialized.task_spec)
                                         : routed_seed.calendar_ref;
    materialized.seed.coverage_report = routed_seed.coverage_report;
    materialized.seed.seeded_components = routed_seed.seeded_components;
    materialized.seed.enabled_components = routed_seed.enabled_components;
    materialized.seed.theta_init = routed_seed.theta_init;
    materialized.seed.monthpos_hint = routed_seed.monthpos_hint;
    materialized.seed.event_hint = routed_seed.event_hint;
    materialized.seed.sigma_init = routed_seed.sigma_init;
    materialized.seed.ratio_prior_init = routed_seed.ratio_prior_init;
    materialized.seed.uncertainty_init = routed_seed.uncertainty_init;
    materialized.seed.maturity_init = routed_seed.maturity_init;

    *out = std::move(materialized);
    return BaselineStatus::kOk;
}

}  // namespace baseline
}  // namespace flowsql
