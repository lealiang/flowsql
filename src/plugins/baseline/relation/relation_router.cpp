/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/relation/relation_router.h"

#include <common/error_code.h>

#include <string>

namespace flowsql {
namespace baseline {

namespace {

std::string MakeFeatureId(const RelationTaskSpec& spec,
                          const std::string& metric_name,
                          const char* suffix) {
    return spec.feature_base + "_" + metric_name + "_" + suffix;
}

}  // namespace

void RelationRouter::BuildRoutedFeatureSpecs(
    const RelationTaskSpec& spec,
    std::vector<RelationRoutedFeatureSpec>* out_specs) {
    if (!out_specs) return;
    out_specs->clear();

    for (const auto& metric_name : spec.metrics) {
        out_specs->push_back(RelationRoutedFeatureSpec{
            metric_name,
            MakeFeatureId(spec, metric_name, "entropy_shannon"),
            RelationRoutedDetectorKind::kValue,
            RelationSummaryKind::kEntropyShannon,
            "t1a",
            "default",
            -1});
        out_specs->push_back(RelationRoutedFeatureSpec{
            metric_name,
            MakeFeatureId(spec, metric_name, "top1_share"),
            RelationRoutedDetectorKind::kRatio,
            RelationSummaryKind::kTop1Share,
            "t2",
            "rate_core",
            -1});
        out_specs->push_back(RelationRoutedFeatureSpec{
            metric_name,
            MakeFeatureId(spec, metric_name, "headk_share"),
            RelationRoutedDetectorKind::kRatio,
            RelationSummaryKind::kHeadKShare,
            "t2",
            "rate_core",
            -1});
        out_specs->push_back(RelationRoutedFeatureSpec{
            metric_name,
            MakeFeatureId(spec, metric_name, "out_of_support_share"),
            RelationRoutedDetectorKind::kRatio,
            RelationSummaryKind::kOutOfSupportShare,
            "t2",
            "ratio_bursty",
            -1});
        out_specs->push_back(RelationRoutedFeatureSpec{
            metric_name,
            MakeFeatureId(spec, metric_name, "distinct_group_count"),
            RelationRoutedDetectorKind::kValue,
            RelationSummaryKind::kDistinctGroupCount,
            "t1a",
            "default",
            -1});
        for (int i = 0; i < spec.summary_policy.k_stable; ++i) {
            out_specs->push_back(RelationRoutedFeatureSpec{
                metric_name,
                MakeFeatureId(
                    spec,
                    metric_name,
                    ("stable_g" + std::to_string(i + 1) + "_share").c_str()),
                RelationRoutedDetectorKind::kRatio,
                RelationSummaryKind::kStableGShare,
                "t2",
                "rate_core",
                i});
        }
        out_specs->push_back(RelationRoutedFeatureSpec{
            metric_name,
            MakeFeatureId(spec, metric_name, "stable_head_coverage"),
            RelationRoutedDetectorKind::kRatio,
            RelationSummaryKind::kStableHeadCoverage,
            "t2",
            "rate_core",
            -1});
        out_specs->push_back(RelationRoutedFeatureSpec{
            metric_name,
            MakeFeatureId(spec, metric_name, "stable_head_mix_drift"),
            RelationRoutedDetectorKind::kValue,
            RelationSummaryKind::kStableHeadMixDrift,
            "t1a",
            "default",
            -1});
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
