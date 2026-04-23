/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <cassert>
#include <cmath>
#include <cstdio>

#include <common/error_code.h>
#include <plugins/baseline/config_parser.h>
#include <plugins/baseline/relation/relation_basis.h>
#include <plugins/baseline/relation/relation_router.h>
#include <plugins/baseline/relation/relation_summary_extractor.h>

using namespace flowsql;
using namespace flowsql::baseline;

namespace {

bool NearlyEqual(double lhs, double rhs, double eps = 1e-9) {
    return std::fabs(lhs - rhs) <= eps;
}

void TestParseRelationTaskSpec() {
    std::printf("[TEST] Relation task spec parsing...\n");

    const char* config_json = R"JSON(
{
  "name": "client_group_mix",
  "feature_base": "client_group_mix",
  "group_space_id": "client_group",
  "group_space_version": "v1",
  "metric_set_id": "traffic",
  "metrics": ["conn_count", "bps", "pps"],
  "encode_type": "topk_other",
  "support_policy": {
    "k_support": 8,
    "min_hist_share": 0.005,
    "min_active_ratio": 0.2
  },
  "summary_policy": {
    "k_head": 5,
    "k_stable": 3
  }
}
)JSON";

    RelationTaskSpec spec;
    std::string err;
    assert(ConfigParser::ParseRelationTask(config_json, &spec, &err) == error::OK);
    assert(spec.feature_base == "client_group_mix");
    assert(spec.group_space_id == "client_group");
    assert(spec.group_space_version == "v1");
    assert(spec.metric_set_id == "traffic");
    assert(spec.metrics.size() == 3);
    assert(spec.metrics[0] == "conn_count");
    assert(spec.metrics[1] == "bps");
    assert(spec.metrics[2] == "pps");
    assert(spec.encode_type == "topk_other");
    assert(spec.support_policy.k_support == 8);
    assert(NearlyEqual(spec.support_policy.min_hist_share, 0.005));
    assert(NearlyEqual(spec.support_policy.min_active_ratio, 0.2));
    assert(spec.summary_policy.k_head == 5);
    assert(spec.summary_policy.k_stable == 3);

    std::printf("[PASS] Relation task spec parsing\n");
}

void TestBuildRelationBasisAndEvalBasis() {
    std::printf("[TEST] Relation basis build and eval basis...\n");

    RelationBasisBuildInput input;
    input.basis_version = 3;
    input.feature_base = "client_group_mix";
    input.metric_name = "bps";
    input.group_space_id = "client_group";
    input.group_space_version = "v1";
    input.support_policy.k_support = 3;
    input.support_policy.min_hist_share = 0.10;
    input.support_policy.min_active_ratio = 0.20;
    input.summary_policy.k_head = 2;
    input.summary_policy.k_stable = 2;
    input.valid_bucket_count = 10;
    input.group_stats = {
        {11, 60.0, 10},
        {12, 25.0, 6},
        {13, 10.0, 3},
        {14, 5.0, 1},
    };

    RelationServiceBasis basis;
    assert(RelationBasisBuilder::BuildServiceBasis(input, &basis) == error::OK);
    assert(basis.basis_version == 3);
    assert(basis.k_head == 2);
    assert(basis.support_explicit.size() == 3);
    assert(basis.support_explicit[0] == 11);
    assert(basis.support_explicit[1] == 12);
    assert(basis.support_explicit[2] == 13);
    assert(basis.stable_head.size() == 2);
    assert(basis.stable_head[0] == 11);
    assert(basis.stable_head[1] == 12);
    assert(basis.head_proto_q.size() == 2);
    assert(NearlyEqual(basis.head_proto_q[0], 60.0 / 85.0));
    assert(NearlyEqual(basis.head_proto_q[1], 25.0 / 85.0));

    RelationTaskSpec same_spec;
    same_spec.feature_base = "client_group_mix";
    same_spec.group_space_id = "client_group";
    same_spec.group_space_version = "v1";
    RelationEvalBasis eval_basis;
    assert(RelationBasisBuilder::BuildEvalBasis(&basis, same_spec, &eval_basis) == error::OK);
    assert(eval_basis.has_incumbent == true);
    assert(eval_basis.compatibility == RelationLineageCompatibility::kIdentical);
    assert(eval_basis.basis.support_explicit.size() == 3);

    RelationTaskSpec new_lineage_spec = same_spec;
    new_lineage_spec.group_space_version = "v2";
    assert(RelationBasisBuilder::DetermineCompatibility(&basis, new_lineage_spec) ==
           RelationLineageCompatibility::kNewLineage);

    std::printf("[PASS] Relation basis build and eval basis\n");
}

void TestExtractRelationSummaries() {
    std::printf("[TEST] Relation summary extraction...\n");

    RelationServiceBasis basis;
    basis.basis_version = 1;
    basis.feature_base = "client_group_mix";
    basis.metric_name = "bps";
    basis.group_space_id = "client_group";
    basis.group_space_version = "v1";
    basis.k_head = 2;
    basis.support_explicit = {11, 12, 13};
    basis.stable_head = {11, 12};
    basis.head_proto_q = {0.75, 0.25};

    const uint32_t group_idx[] = {11, 12, 50};
    const double values_exact[] = {50.0, 30.0, 20.0};
    const RelationMetricBlock metrics_exact[] = {
        {100.0, 3, values_exact},
    };
    const RelationObservationBlock block_exact{
        BaselineStringRef{"svc-a", 5},
        10,
        3,
        group_idx,
        1,
        metrics_exact};

    RelationMetricSummary summary_exact;
    assert(RelationSummaryExtractor::ExtractMetricSummary(
               block_exact, 0, basis, &summary_exact) == error::OK);
    assert(summary_exact.valid == true);
    assert(NearlyEqual(summary_exact.top1_share, 0.5));
    assert(NearlyEqual(summary_exact.headk_share, 0.8));
    assert(NearlyEqual(summary_exact.out_of_support_share, 0.2));
    assert(summary_exact.has_distinct_group_count == true);
    assert(NearlyEqual(summary_exact.distinct_group_count, 3.0));
    assert(summary_exact.stable_g_shares.size() == 2);
    assert(NearlyEqual(summary_exact.stable_g_shares[0], 0.5));
    assert(NearlyEqual(summary_exact.stable_g_shares[1], 0.3));
    assert(NearlyEqual(summary_exact.stable_headk_coverage, 0.8));
    assert(summary_exact.has_stable_headk_mix_drift == true);
    assert(NearlyEqual(summary_exact.stable_headk_mix_drift, 0.125));

    const double values_topk_other[] = {40.0, 30.0, 20.0};
    const RelationMetricBlock metrics_topk_other[] = {
        {100.0, 0, values_topk_other},
    };
    const RelationObservationBlock block_topk_other{
        BaselineStringRef{"svc-a", 5},
        11,
        3,
        group_idx,
        1,
        metrics_topk_other};

    RelationMetricSummary summary_topk_other;
    assert(RelationSummaryExtractor::ExtractMetricSummary(
               block_topk_other, 0, basis, &summary_topk_other) == error::OK);
    assert(summary_topk_other.valid == true);
    assert(NearlyEqual(summary_topk_other.top1_share, 0.4));
    assert(NearlyEqual(summary_topk_other.headk_share, 0.7));
    assert(NearlyEqual(summary_topk_other.out_of_support_share, 0.3));
    assert(summary_topk_other.has_distinct_group_count == false);

    std::printf("[PASS] Relation summary extraction\n");
}

void TestRelationRouter() {
    std::printf("[TEST] Relation routed feature mapping...\n");

    RelationTaskSpec spec;
    spec.feature_base = "client_group_mix";
    spec.metrics = {"conn_count"};
    spec.summary_policy.k_stable = 2;

    std::vector<RelationRoutedFeatureSpec> routed_specs;
    RelationRouter::BuildRoutedFeatureSpecs(spec, &routed_specs);
    assert(!routed_specs.empty());

    bool found_entropy = false;
    bool found_stable_g2 = false;
    for (const auto& routed_spec : routed_specs) {
        if (routed_spec.routed_feature_id ==
            "client_group_mix_conn_count_entropy_shannon") {
            found_entropy = true;
        }
        if (routed_spec.routed_feature_id ==
            "client_group_mix_conn_count_stable_g2_share") {
            found_stable_g2 = true;
        }
    }
    assert(found_entropy == true);
    assert(found_stable_g2 == true);

    RelationMetricSummary summary;
    summary.valid = true;
    summary.total = 100.0;
    summary.entropy_shannon = 0.9;
    summary.top1_share = 0.4;
    summary.headk_share = 0.7;
    summary.out_of_support_share = 0.3;
    summary.has_distinct_group_count = true;
    summary.distinct_group_count = 12.0;
    summary.stable_g_shares = {0.25, 0.10};
    summary.stable_headk_coverage = 0.35;
    summary.has_stable_headk_mix_drift = true;
    summary.stable_headk_mix_drift = 0.2;

    ValueObservation value_observation;
    assert(RelationRouter::BuildValueObservation(
               routed_specs[0], BaselineStringRef{"svc-a", 5}, 20, summary,
               &value_observation) == true);

    bool found_ratio = false;
    for (const auto& routed_spec : routed_specs) {
        if (routed_spec.routed_feature_id ==
            "client_group_mix_conn_count_top1_share") {
            RatioObservation ratio_observation;
            assert(RelationRouter::BuildRatioObservation(
                       routed_spec, BaselineStringRef{"svc-a", 5}, 20, summary,
                       &ratio_observation) == true);
            assert(NearlyEqual(ratio_observation.numerator, 40.0));
            assert(NearlyEqual(ratio_observation.denominator, 100.0));
            found_ratio = true;
        }
    }
    assert(found_ratio == true);

    std::printf("[PASS] Relation routed feature mapping\n");
}

}  // namespace

int main() {
    TestParseRelationTaskSpec();
    TestBuildRelationBasisAndEvalBasis();
    TestExtractRelationSummaries();
    TestRelationRouter();
    std::printf("[DONE] test_baseline_relation_task\n");
    return 0;
}
