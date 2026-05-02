/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_BOOTSTRAP_BOOTSTRAP_TYPES_H_
#define _FLOWSQL_PLUGINS_BASELINE_BOOTSTRAP_BOOTSTRAP_TYPES_H_

#include <framework/interfaces/ibaseline_types.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "plugins/baseline/model/formal_model.h"
#include "plugins/baseline/relation/relation_basis.h"

namespace flowsql {
namespace baseline {

enum class BootstrapArtifactKind : int32_t {
    kNone = 0,
    kValue = 1,
    kRatio = 2,
    kRelation = 3,
};

struct BootstrapCoverageReport {
    uint64_t accepted_count = 0;
    uint64_t rejected_count = 0;
    int64_t train_start_bucket = 0;
    int64_t train_end_bucket = 0;
    double coverage_ratio = 0.0;
};

struct BootstrapTaskIdentity {
    std::string task_id;
    std::string task_kind;
    std::string feature_type;
    std::string feature_id;
    std::string profile;
};

struct BootstrapClockSpec {
    int64_t bucket_seconds = 0;
    std::string timezone;
};

struct BootstrapCalendarRef {
    std::string calendar_id;
    std::string calendar_version;
};

struct BootstrapHarmonicInit {
    int32_t order = 0;
    double sin = 0.0;
    double cos = 0.0;
};

struct BootstrapThetaInit {
    bool available = false;
    std::string model_space;
    int64_t reference_bucket_id = 0;
    double level = 0.0;
    double trend = 0.0;
    std::vector<BootstrapHarmonicInit> daily_harmonic;
    std::vector<BootstrapHarmonicInit> weekly_harmonic;
};

struct BootstrapSigmaInit {
    bool available = false;
    double value = 0.0;
    std::string model_space;
    std::string source;
};

struct BootstrapRatioPriorInit {
    bool available = false;
    double m0 = 0.5;
    double alpha0 = 0.0;
    double beta0 = 0.0;
    std::string model_space = "probability";
    std::string source;
};

struct BootstrapMonthPosHint {
    bool available = false;
    std::vector<double> dom_coeff;
    std::vector<double> dme_coeff;
    std::vector<double> lwd_coeff;
    std::vector<double> dom_center;
    std::vector<double> dme_center;
    std::vector<double> lwd_center;
};

struct BootstrapEventHint {
    bool available = false;
    std::string calendar_id;
    std::string calendar_version;
    std::vector<std::string> active_event_codes;
    std::vector<double> coeff;
};

struct BootstrapSeedQualityConfig {
    double full_min_coverage_ratio = 0.90;
    double partial_min_coverage_ratio = 0.50;
    uint32_t daily_min_span_days = 1;
    uint32_t weekly_min_span_days = 14;
    double daily_phase_coverage_ratio = 0.75;
    double weekly_phase_coverage_ratio = 0.70;
};

struct BootstrapComponentUncertaintyInit {
    double level_scale = 1.0;
    double trend_scale = 4.0;
    double daily_scale = 2.0;
    double weekly_scale = 4.0;
};

struct BootstrapUncertaintyInit {
    bool available = false;
    double confidence_base = 0.0;
    double confidence_level = 0.95;
    double coverage_ratio = 0.0;
    double band_z = 1.96;
    std::string band_source;
    std::vector<std::string> uncertainty_source;
    BootstrapComponentUncertaintyInit component_uncertainty;
};

struct BootstrapMaturityInit {
    bool available = false;
    BootstrapSeedStatus seed_status = BootstrapSeedStatus::kNone;
    double confidence = 0.0;
    uint64_t accepted_count = 0;
    uint64_t rejected_count = 0;
    double coverage_ratio = 0.0;
};

struct BootstrapRelationBasisSeed {
    uint64_t basis_version = 0;
    std::string feature_base;
    std::string metric_name;
    std::string group_space_id;
    std::string group_space_version;
    int32_t k_head = 0;
    std::vector<uint32_t> other_group_idxs;
    std::vector<uint32_t> support_explicit;
    std::vector<uint32_t> stable_head;
    std::vector<double> head_proto_q;
};

struct RelationRoutedBootstrapSeed {
    std::string metric_name;
    std::string summary_name;
    BaselineTaskKind task_kind = BaselineTaskKind::kValue;
    BootstrapTaskIdentity task_identity;
    BootstrapClockSpec clock_spec;
    BootstrapCalendarRef calendar_ref;
    BootstrapCoverageReport coverage_report;
    BootstrapSeedStatus seed_status = BootstrapSeedStatus::kNone;
    std::vector<std::string> seeded_components;
    std::vector<std::string> enabled_components;
    BootstrapThetaInit theta_init;
    BootstrapMonthPosHint monthpos_hint;
    BootstrapEventHint event_hint;
    BootstrapSigmaInit sigma_init;
    BootstrapRatioPriorInit ratio_prior_init;
    BootstrapUncertaintyInit uncertainty_init;
    BootstrapMaturityInit maturity_init;
};

struct BootstrapSeed {
    BootstrapArtifactKind artifact_kind = BootstrapArtifactKind::kNone;
    BootstrapSeedStatus seed_status = BootstrapSeedStatus::kNone;
    uint64_t source_artifact_version = 0;
    std::string series_key;
    BootstrapTaskIdentity task_identity;
    BootstrapClockSpec clock_spec;
    BootstrapCalendarRef calendar_ref;
    BootstrapCoverageReport coverage_report;
    std::vector<std::string> seeded_components;
    std::vector<std::string> enabled_components;
    BootstrapThetaInit theta_init;
    BootstrapMonthPosHint monthpos_hint;
    BootstrapEventHint event_hint;
    BootstrapSigmaInit sigma_init;
    BootstrapRatioPriorInit ratio_prior_init;
    BootstrapUncertaintyInit uncertainty_init;
    BootstrapMaturityInit maturity_init;
    std::vector<BootstrapRelationBasisSeed> relation_basis_by_metric;
    std::vector<RelationRoutedBootstrapSeed> relation_routed_summary_seeds;
    std::string diagnostics;
};

struct RelationRoutedBootstrapArtifact {
    std::string metric_name;
    std::string summary_name;
    BaselineTaskKind task_kind = BaselineTaskKind::kValue;
    BootstrapTaskIdentity task_identity;
    BootstrapClockSpec clock_spec;
    BootstrapCalendarRef calendar_ref;
    BootstrapCoverageReport coverage_report;
    std::vector<std::string> seeded_components;
    std::vector<std::string> enabled_components;
    std::shared_ptr<ValueFormalModel> value_model;
    std::shared_ptr<RatioFormalModel> ratio_model;
    std::string diagnostics;
};

struct BootstrapArtifact {
    BootstrapArtifactKind artifact_kind = BootstrapArtifactKind::kNone;
    BaselineStatus train_status = BaselineStatus::kNotTrained;
    uint64_t model_version = 0;
    std::string series_key;
    BootstrapTaskIdentity task_identity;
    BootstrapClockSpec clock_spec;
    BootstrapCalendarRef calendar_ref;
    BootstrapCoverageReport coverage_report;
    std::vector<std::string> seeded_components;
    std::vector<std::string> enabled_components;
    std::shared_ptr<ValueFormalModel> value_model;
    std::shared_ptr<RatioFormalModel> ratio_model;
    std::vector<RelationServiceBasis> relation_basis_by_metric;
    std::vector<RelationRoutedBootstrapArtifact> relation_routed_summary_artifacts;
    std::string diagnostics;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_BOOTSTRAP_BOOTSTRAP_TYPES_H_
