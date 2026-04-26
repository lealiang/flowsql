/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <rapidjson/document.h>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include <common/error_code.h>
#include <framework/interfaces/ibaseline_service.h>
#include <plugins/baseline/config_parser.h>
#include <plugins/baseline/fusion/key_risk_fusion.h>
#include <plugins/baseline/relation/relation_basis.h>
#include <plugins/baseline/relation/relation_router.h>
#include <plugins/baseline/relation/relation_summary_extractor.h>
#include <plugins/baseline/task/relation_task.h>

#include "relation_task_test_access.h"

using namespace flowsql;
using namespace flowsql::baseline;

namespace {

bool NearlyEqual(double lhs, double rhs, double eps = 1e-9) {
    return std::fabs(lhs - rhs) <= eps;
}

rapidjson::Document ParseJson(const std::string& json) {
    rapidjson::Document doc;
    doc.Parse(json.c_str());
    assert(!doc.HasParseError());
    assert(doc.IsObject());
    return doc;
}

class TestRelationSourceResolver : public IBaselineSourceResolver {
 public:
    explicit TestRelationSourceResolver(
        std::string config_json = R"({"baseline_sources":[{"source_key":"svc-source"}]})")
        : config_json_(std::move(config_json)) {}

    int ResolveBaselineSource(const BaselineStringRef& key,
                              const BaselineStringRef& feature,
                              std::string* out_config_json) override {
        last_key_ = key.data ? std::string(key.data, key.size) : "";
        last_feature_ = feature.data ? std::string(feature.data, feature.size) : "";
        ++call_count_;
        if (out_config_json) *out_config_json = config_json_;
        return error::OK;
    }

    int call_count() const { return call_count_; }
    const std::string& last_key() const { return last_key_; }
    const std::string& last_feature() const { return last_feature_; }

 private:
    int call_count_ = 0;
    std::string last_key_;
    std::string last_feature_;
    std::string config_json_;
};

RelationTaskSpec BuildRelationTaskSpecForEncodeValidation(const std::string& task_id,
                                                          const std::string& encode_type) {
    RelationTaskSpec spec;
    spec.task_id = task_id;
    spec.name = "client_group_mix";
    spec.feature_base = "client_group_mix";
    spec.group_space_id = "client_group";
    spec.group_space_version = "v1";
    spec.metric_set_id = "net_metrics";
    spec.metrics = {"conn_count"};
    spec.encode_type = encode_type;
    spec.support_policy.k_support = 8;
    spec.support_policy.min_hist_share = 0.005;
    spec.support_policy.min_active_ratio = 0.2;
    spec.summary_policy.k_head = 2;
    spec.summary_policy.k_stable = 2;
    return spec;
}

void TestProtocolContract() {
    std::printf("[TEST] Baseline protocol contract...\n");

    using SubmitBlockSig =
        int (IBaselineRelationTask::*)(const RelationObservationBlock&, FusionResult*);
    static_assert(std::is_same_v<decltype(&IBaselineRelationTask::SubmitBlock),
                                 SubmitBlockSig>);

    using QueryFusionSig =
        int (IBaselineService::*)(const BaselineStringRef&, std::string*) const;
    static_assert(std::is_same_v<decltype(&IBaselineService::QueryKeyFusionSnapshotJson),
                                 QueryFusionSig>);

    HistoryFetchRequest fetch_req;
    fetch_req.key = BaselineStringRef{"svc-a", 5};
    fetch_req.feature = BaselineStringRef{"bytes_total", 11};
    fetch_req.bucket_start = 100;
    fetch_req.bucket_end = 120;

    RelationMetricBlock metric_block;
    metric_block.total = 10.0;
    metric_block.flags = kRelationMetricHasActiveCount;
    metric_block.active_count = 3;

    DetectorResult detector_result;
    detector_result.key = BaselineStringRef{"svc-a", 5};
    detector_result.feature = BaselineStringRef{"bytes_total", 11};
    detector_result.feature_type = BaselineStringRef{"t1a", 3};
    detector_result.ts = 123;
    detector_result.reason_code = BaselineReasonCode::kSpike;
    detector_result.evidence.kind = BaselineEvidenceKind::kValue;
    detector_result.evidence.value.baseline_source_kind = BaselineSourceKind::kSelf;

    FusionResult fusion_result;
    fusion_result.key = BaselineStringRef{"svc-a", 5};
    fusion_result.ts = 123;
    fusion_result.risk = 0.2;
    fusion_result.dominant_single_count = 1;
    fusion_result.dominant_single[0].feature = BaselineStringRef{"bytes_total", 11};
    fusion_result.dominant_pattern_count = 1;
    fusion_result.dominant_pattern[0].pattern = BaselineStringRef{"support_escape", 14};

    (void)fetch_req;
    (void)metric_block;
    (void)detector_result;
    (void)fusion_result;

    std::printf("[PASS] Baseline protocol contract\n");
}

void TestParseValueTaskBaselineSourceConfig() {
    std::printf("[TEST] Value task baseline source config parsing...\n");

    const char* config_json = R"JSON(
{
  "name": "bytes_total",
  "key": "service",
  "feature": "bytes_total",
  "feature_type": "t1a",
  "feature_profile": "traffic",
  "delta": 60,
  "tz": "Asia/Shanghai",
  "baseline_source_configs": [
    {
      "key": "svc-target",
      "baseline_sources": [{"source_key": "svc-source"}]
    }
  ]
}
)JSON";

    BaselineTaskSpec spec;
    std::string err;
    assert(ConfigParser::ParseValueTask(config_json, &spec, &err) == error::OK);
    assert(spec.baseline_source_configs.size() == 1);
    assert(spec.baseline_source_configs[0].key == "svc-target");
    assert(spec.baseline_source_configs[0].config.sources.size() == 1);
    assert(spec.baseline_source_configs[0].config.sources[0].source_key == "svc-source");

    const char* bad_self_source_json = R"JSON(
{
  "name": "bytes_total",
  "key": "svc-self",
  "feature": "bytes_total",
  "feature_type": "t1a",
  "feature_profile": "traffic",
  "delta": 60,
  "tz": "Asia/Shanghai",
  "baseline_source_configs": [
    {
      "key": "svc-self",
      "baseline_sources": [{"source_key": "svc-self"}]
    }
  ]
}
)JSON";

    assert(ConfigParser::ParseValueTask(bad_self_source_json, &spec, &err) ==
           error::BAD_REQUEST);

    const char* duplicate_key_json = R"JSON(
{
  "name": "bytes_total",
  "key": "service",
  "feature": "bytes_total",
  "feature_type": "t1a",
  "feature_profile": "traffic",
  "delta": 60,
  "tz": "Asia/Shanghai",
  "baseline_source_configs": [
    {
      "key": "svc-target",
      "baseline_sources": [{"source_key": "svc-source-a"}]
    },
    {
      "key": "svc-target",
      "baseline_sources": [{"source_key": "svc-source-b"}]
    }
  ]
}
)JSON";

    assert(ConfigParser::ParseValueTask(duplicate_key_json, &spec, &err) ==
           error::BAD_REQUEST);

    const char* duplicate_source_json = R"JSON(
{
  "name": "bytes_total",
  "key": "service",
  "feature": "bytes_total",
  "feature_type": "t1a",
  "feature_profile": "traffic",
  "delta": 60,
  "tz": "Asia/Shanghai",
  "baseline_source_configs": [
    {
      "key": "svc-target",
      "baseline_sources": [
        {"source_key": "svc-source"},
        {"source_key": "svc-source"}
      ]
    }
  ]
}
)JSON";

    assert(ConfigParser::ParseValueTask(duplicate_source_json, &spec, &err) ==
           error::BAD_REQUEST);

    const char* task_global_source_json = R"JSON(
{
  "name": "bytes_total",
  "key": "service",
  "feature": "bytes_total",
  "feature_type": "t1a",
  "feature_profile": "traffic",
  "delta": 60,
  "tz": "Asia/Shanghai",
  "baseline_source_config": [
    {"source_key": "svc-source"}
  ]
}
)JSON";

    assert(ConfigParser::ParseValueTask(task_global_source_json, &spec, &err) ==
           error::BAD_REQUEST);

    const char* legacy_series_override_json = R"JSON(
{
  "name": "bytes_total",
  "key": "service",
  "feature": "bytes_total",
  "feature_type": "t1a",
  "feature_profile": "traffic",
  "delta": 60,
  "tz": "Asia/Shanghai",
  "series_overrides": [
    {
      "key": "svc-target",
      "baseline_sources": [{"source_key": "svc-source"}]
    }
  ]
}
)JSON";

    assert(ConfigParser::ParseValueTask(legacy_series_override_json, &spec, &err) ==
           error::BAD_REQUEST);

    std::printf("[PASS] Value task baseline source config parsing\n");
}

void TestParseRelationTaskSpec() {
    std::printf("[TEST] Relation task spec parsing...\n");

    const char* config_json = R"JSON(
{
  "name": "client_group_mix",
  "feature_base": "client_group_mix",
  "group_space_id": "client_group",
  "group_space_version": "v1",
  "delta": 60,
  "tz": "Asia/Shanghai",
  "metric_set_id": "traffic",
  "metrics": ["conn_count", "bps", "pps"],
  "encode_type": "topk_other",
  "other_group_idx": 7,
  "other_group_idxs": [9, 8],
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

    RelationTaskCreateSpec create_spec;
    std::string err;
    assert(ConfigParser::ParseRelationTask(config_json, &create_spec, &err) == error::OK);
    const RelationTaskSpec& spec = create_spec.task_spec;
    assert(spec.feature_base == "client_group_mix");
    assert(spec.group_space_id == "client_group");
    assert(spec.group_space_version.has_value());
    assert(*spec.group_space_version == "v1");
    assert(create_spec.clock_spec.delta == 60);
    assert(create_spec.clock_spec.tz == "Asia/Shanghai");
    assert(spec.metric_set_id == "traffic");
    assert(spec.metrics.size() == 3);
    assert(spec.metrics[0] == "conn_count");
    assert(spec.metrics[1] == "bps");
    assert(spec.metrics[2] == "pps");
    assert(spec.encode_type == "topk_other");
    assert(spec.other_group_idxs.size() == 3);
    assert(spec.other_group_idxs[0] == 7);
    assert(spec.other_group_idxs[1] == 8);
    assert(spec.other_group_idxs[2] == 9);
    assert(spec.support_policy.k_support == 8);
    assert(NearlyEqual(spec.support_policy.min_hist_share, 0.005));
    assert(NearlyEqual(spec.support_policy.min_active_ratio, 0.2));
    assert(spec.summary_policy.k_head == 5);
    assert(spec.summary_policy.k_stable == 3);

    const char* missing_clock_json = R"JSON(
{
  "name": "client_group_mix",
  "feature_base": "client_group_mix",
  "group_space_id": "client_group",
  "metric_set_id": "traffic",
  "metrics": ["conn_count"],
  "encode_type": "exact_sparse",
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
    assert(ConfigParser::ParseRelationTask(missing_clock_json, &create_spec, &err) ==
           error::BAD_REQUEST);

    const char* duplicate_metric_json = R"JSON(
{
  "name": "client_group_mix",
  "feature_base": "client_group_mix",
  "group_space_id": "client_group",
  "delta": 60,
  "tz": "Asia/Shanghai",
  "metric_set_id": "traffic",
  "metrics": ["conn_count", "conn_count"],
  "encode_type": "exact_sparse",
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
    assert(ConfigParser::ParseRelationTask(duplicate_metric_json, &create_spec, &err) ==
           error::BAD_REQUEST);

    const char* invalid_relation_policy_json = R"JSON(
{
  "name": "client_group_mix",
  "feature_base": "client_group_mix",
  "group_space_id": "client_group",
  "delta": 60,
  "tz": "Asia/Shanghai",
  "metric_set_id": "traffic",
  "metrics": ["conn_count"],
  "encode_type": "exact_sparse",
  "support_policy": {
    "k_support": 2,
    "min_hist_share": 0.005,
    "min_active_ratio": 0.2
  },
  "summary_policy": {
    "k_head": 5,
    "k_stable": 3
  }
}
)JSON";
    assert(ConfigParser::ParseRelationTask(invalid_relation_policy_json,
                                           &create_spec,
                                           &err) == error::BAD_REQUEST);

    const char* invalid_other_for_exact_sparse_json = R"JSON(
{
  "name": "client_group_mix",
  "feature_base": "client_group_mix",
  "group_space_id": "client_group",
  "delta": 60,
  "tz": "Asia/Shanghai",
  "metric_set_id": "traffic",
  "metrics": ["conn_count"],
  "encode_type": "exact_sparse",
  "other_group_idxs": [9],
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
    assert(ConfigParser::ParseRelationTask(invalid_other_for_exact_sparse_json,
                                           &create_spec,
                                           &err) == error::BAD_REQUEST);

    std::printf("[PASS] Relation task spec parsing\n");
}

void TestRelationEncodeTypeValidationOnSubmit() {
    std::printf("[TEST] Relation block encode_type validation...\n");

    const uint32_t group_idx[] = {11, 12};
    const double values[] = {40.0, 30.0};
    const RelationMetricBlock metrics[] = {
        {100.0, 0, 0, values},
    };
    const RelationObservationBlock block{
        BaselineStringRef{"svc-a", 5},
        100,
        2,
        group_idx,
        1,
        metrics};

    RelationTaskClockSpec clock_spec;
    clock_spec.delta = 60;
    clock_spec.tz = "UTC";
    KeyRiskFusion fusion;

    const RelationTaskSpec exact_spec =
        BuildRelationTaskSpecForEncodeValidation("relation-encode-exact", "exact_sparse");
    BaselineRelationTask exact_task(
        nullptr, nullptr, exact_spec.task_id, exact_spec, clock_spec, std::nullopt, nullptr, &fusion);
    FusionResult result{};
    assert(exact_task.SubmitBlock(block, &result) == error::BAD_REQUEST);

    const RelationTaskSpec topk_spec =
        BuildRelationTaskSpecForEncodeValidation("relation-encode-topk", "topk_other");
    BaselineRelationTask topk_task(
        nullptr, nullptr, topk_spec.task_id, topk_spec, clock_spec, std::nullopt, nullptr, &fusion);
    assert(topk_task.SubmitBlock(block, &result) == error::OK);

    std::printf("[PASS] Relation block encode_type validation\n");
}

void TestRelationOtherGroupIdxValidationOnSubmit() {
    std::printf("[TEST] Relation other_group_idxs validation on submit...\n");

    const uint32_t group_idx[] = {11, 77};
    const double values[] = {60.0, 40.0};
    const RelationMetricBlock metrics[] = {
        {100.0, 0, 0, values},
    };
    const RelationObservationBlock block{
        BaselineStringRef{"svc-a", 5},
        101,
        2,
        group_idx,
        1,
        metrics};

    RelationTaskClockSpec clock_spec;
    clock_spec.delta = 60;
    clock_spec.tz = "UTC";
    KeyRiskFusion fusion;
    FusionResult result{};

    RelationTaskSpec exact_spec =
        BuildRelationTaskSpecForEncodeValidation("relation-other-exact", "exact_sparse");
    exact_spec.other_group_idxs = {77};
    BaselineRelationTask exact_task(
        nullptr, nullptr, exact_spec.task_id, exact_spec, clock_spec, std::nullopt, nullptr, &fusion);
    assert(exact_task.SubmitBlock(block, &result) == error::BAD_REQUEST);

    RelationTaskSpec topk_spec =
        BuildRelationTaskSpecForEncodeValidation("relation-other-topk", "topk_other");
    topk_spec.other_group_idxs = {77};
    BaselineRelationTask topk_task(
        nullptr, nullptr, topk_spec.task_id, topk_spec, clock_spec, std::nullopt, nullptr, &fusion);
    assert(topk_task.SubmitBlock(block, &result) == error::OK);

    std::printf("[PASS] Relation other_group_idxs validation on submit\n");
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

void TestBuildRelationBasisExcludeOtherGroupIdxs() {
    std::printf("[TEST] Relation basis excludes other_group_idxs...\n");

    RelationBasisBuildInput input;
    input.basis_version = 4;
    input.feature_base = "client_group_mix";
    input.metric_name = "bps";
    input.group_space_id = "client_group";
    input.group_space_version = "v1";
    input.other_group_idxs = {77};
    input.support_policy.k_support = 3;
    input.support_policy.min_hist_share = 0.05;
    input.support_policy.min_active_ratio = 0.20;
    input.summary_policy.k_head = 2;
    input.summary_policy.k_stable = 2;
    input.valid_bucket_count = 10;
    input.group_stats = {
        {77, 70.0, 10},
        {11, 20.0, 10},
        {12, 10.0, 6},
    };

    RelationServiceBasis basis;
    assert(RelationBasisBuilder::BuildServiceBasis(input, &basis) == error::OK);
    assert(basis.other_group_idxs.size() == 1);
    assert(basis.other_group_idxs[0] == 77);
    assert(std::find(basis.support_explicit.begin(),
                     basis.support_explicit.end(),
                     77) == basis.support_explicit.end());
    assert(basis.support_explicit.size() == 2);
    assert(basis.support_explicit[0] == 11);
    assert(basis.support_explicit[1] == 12);

    std::printf("[PASS] Relation basis excludes other_group_idxs\n");
}

void TestRelationTaskSeedMetricBasisViaTestAccess() {
    std::printf("[TEST] Relation task seed metric basis via test access...\n");

    RelationTaskSpec spec;
    spec.task_id = "relation-seed";
    spec.name = "client_group_mix";
    spec.feature_base = "client_group_mix";
    spec.group_space_id = "client_group";
    spec.group_space_version = "v2";
    spec.metric_set_id = "net_metrics";
    spec.metrics = {"conn_count"};
    spec.encode_type = "exact_sparse";
    spec.support_policy.k_support = 8;
    spec.support_policy.min_hist_share = 0.01;
    spec.support_policy.min_active_ratio = 0.2;
    spec.summary_policy.k_head = 2;
    spec.summary_policy.k_stable = 2;

    RelationTaskClockSpec clock_spec;
    clock_spec.delta = 60;
    clock_spec.tz = "UTC";

    KeyRiskFusion fusion;
    BaselineRelationTask task(
        nullptr, nullptr, "relation-seed", spec, clock_spec, std::nullopt, nullptr, &fusion);

    RelationServiceBasis basis;
    basis.basis_version = 7;
    basis.feature_base = "client_group_mix";
    basis.metric_name = "conn_count";
    basis.group_space_id = "client_group";
    basis.group_space_version = "v1";
    basis.k_head = 2;
    basis.support_explicit = {11, 12, 13};
    basis.stable_head = {11, 12};
    basis.head_proto_q = {0.8, 0.2};

    RelationTaskTestAccess::SeedMetricBasis(&task, "svc-new-lineage", "conn_count", basis);

    std::string snapshot;
    assert(task.QuerySeriesSnapshotJson(BaselineStringRef{"svc-new-lineage", 15}, &snapshot) ==
           error::OK);
    auto doc = ParseJson(snapshot);
    assert(doc["basis_metric_count"].GetUint64() == 1);
    assert(doc["metrics"].IsArray());
    assert(doc["metrics"].Size() == 1);
    assert(doc["metrics"][0]["basis_version"].GetUint64() == 7);
    assert(std::string(doc["metrics"][0]["service_basis"]["group_space_version"].GetString()) ==
           "v1");

    std::printf("[PASS] Relation task seed metric basis via test access\n");
}

void TestKeyRiskFusionRemoveTaskContributionsByTaskId() {
    std::printf("[TEST] KeyRiskFusion remove task contributions by task id...\n");

    KeyRiskFusion fusion;
    const std::string key = "svc-fusion-mixed";
    const int64_t ts = 42;

    DetectorResult direct_result;
    direct_result.status = error::OK;
    direct_result.key = BaselineStringRef{key.c_str(), static_cast<uint32_t>(key.size())};
    direct_result.feature = BaselineStringRef{"bytes_total", 11};
    direct_result.ts = ts;
    direct_result.normalized_score = 0.9;
    direct_result.confidence = 1.0;
    direct_result.persistence = 3;
    direct_result.direction = BaselineDirection::kUp;
    direct_result.reason_code = BaselineReasonCode::kBaselineShiftUp;

    DetectorResult routed_result = direct_result;
    routed_result.feature = BaselineStringRef{"client_group_mix_conn_count_entropy_shannon", 43};
    routed_result.normalized_score = 0.8;
    routed_result.reason_code = BaselineReasonCode::kSpike;

    FusionResult pattern_result;
    pattern_result.key = BaselineStringRef{key.c_str(), static_cast<uint32_t>(key.size())};
    pattern_result.ts = ts;
    pattern_result.risk = 0.7;
    pattern_result.dominant_pattern_count = 1;
    pattern_result.dominant_pattern[0].pattern = BaselineStringRef{"support_escape", 14};
    pattern_result.dominant_pattern[0].feature_base = BaselineStringRef{"client_group_mix", 16};
    pattern_result.dominant_pattern[0].score_pattern = 0.7;
    pattern_result.dominant_pattern[0].metrics_hit_count = 1;
    pattern_result.dominant_pattern[0].metrics_hit[0] = BaselineStringRef{"conn_count", 10};
    pattern_result.dominant_pattern[0].supporting_feature_count = 1;
    pattern_result.dominant_pattern[0].supporting_features[0] =
        BaselineStringRef{"client_group_mix_conn_count_entropy_shannon", 43};

    fusion.UpdateSingleDetectorResult(
        ts, FusionSourceId{"value-task", FusionSourceKind::kDirectSingle, 0}, direct_result);
    fusion.UpdateSingleDetectorResult(
        ts, FusionSourceId{"relation-task", FusionSourceKind::kRoutedSingle, 1}, routed_result);
    fusion.UpdateRelationFusionResult(
        ts, FusionSourceId{"relation-task", FusionSourceKind::kRelationPattern, 2}, pattern_result);

    KeyRiskFusionSnapshot snapshot;
    assert(fusion.QueryKeyFusionSnapshot(key, &snapshot) == error::OK);
    assert(snapshot.available);
    assert(snapshot.active_window.available);
    assert(snapshot.active_window.ts == ts);
    assert(snapshot.active_window.dominant_single_count == 2);
    assert(snapshot.active_window.dominant_pattern_count == 1);

    fusion.RemoveTaskContributions("value-task");
    assert(fusion.QueryKeyFusionSnapshot(key, &snapshot) == error::OK);
    assert(snapshot.available);
    assert(snapshot.active_window.available);
    assert(snapshot.active_window.dominant_single_count == 1);
    assert(snapshot.active_window.dominant_singles[0].feature ==
           "client_group_mix_conn_count_entropy_shannon");
    assert(snapshot.active_window.dominant_pattern_count == 1);

    fusion.RemoveTaskContributions("relation-task");
    std::string snapshot_json;
    assert(fusion.QueryKeyFusionSnapshotJson(
               BaselineStringRef{key.c_str(), static_cast<uint32_t>(key.size())},
               &snapshot_json) == error::OK);
    auto doc = ParseJson(snapshot_json);
    assert(doc["available"].GetBool() == false);

    std::printf("[PASS] KeyRiskFusion remove task contributions by task id\n");
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
        {100.0, kRelationMetricHasActiveCount, 3, values_exact},
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
        {100.0, 0, 0, values_topk_other},
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

    const double values_zero_active[] = {60.0, 25.0, 15.0};
    const RelationMetricBlock metrics_zero_active[] = {
        {100.0, kRelationMetricHasActiveCount, 0, values_zero_active},
    };
    const RelationObservationBlock block_zero_active{
        BaselineStringRef{"svc-a", 5},
        12,
        3,
        group_idx,
        1,
        metrics_zero_active};

    RelationMetricSummary summary_zero_active;
    assert(RelationSummaryExtractor::ExtractMetricSummary(
               block_zero_active, 0, basis, &summary_zero_active) == error::OK);
    assert(summary_zero_active.has_distinct_group_count == true);
    assert(NearlyEqual(summary_zero_active.distinct_group_count, 0.0));

    std::printf("[PASS] Relation summary extraction\n");
}

void TestExtractRelationSummariesExcludeOtherGroupIdxs() {
    std::printf("[TEST] Relation summary excludes other_group_idxs from top/support...\n");

    RelationServiceBasis basis;
    basis.basis_version = 1;
    basis.feature_base = "client_group_mix";
    basis.metric_name = "bps";
    basis.group_space_id = "client_group";
    basis.group_space_version = "v1";
    basis.k_head = 2;
    basis.other_group_idxs = {77};
    // 故意把 77 放进 support_explicit，验证提取阶段仍会排除该聚合槽位。
    basis.support_explicit = {11, 12, 77};
    basis.stable_head = {11, 12};
    basis.head_proto_q = {0.5, 0.5};

    const uint32_t group_idx[] = {11, 50, 77};
    const double values[] = {30.0, 10.0, 60.0};
    const RelationMetricBlock metrics[] = {
        {100.0, 0, 0, values},
    };
    const RelationObservationBlock block{
        BaselineStringRef{"svc-a", 5},
        21,
        3,
        group_idx,
        1,
        metrics};

    RelationMetricSummary summary;
    assert(RelationSummaryExtractor::ExtractMetricSummary(block, 0, basis, &summary) == error::OK);
    assert(summary.valid == true);
    assert(NearlyEqual(summary.top1_share, 0.3));
    assert(NearlyEqual(summary.headk_share, 0.4));
    assert(NearlyEqual(summary.out_of_support_share, 0.7));
    assert(summary.stable_g_shares.size() == 2);
    assert(NearlyEqual(summary.stable_g_shares[0], 0.3));
    assert(NearlyEqual(summary.stable_g_shares[1], 0.0));

    std::printf("[PASS] Relation summary excludes other_group_idxs from top/support\n");
}

void TestRelationRouter() {
    std::printf("[TEST] Relation routed feature mapping...\n");

    RelationTaskSpec spec;
    spec.feature_base = "client_group_mix";
    spec.metrics = {"conn_count"};
    spec.summary_policy.k_stable = 3;

    RelationTaskClockSpec clock_spec;
    clock_spec.delta = 60;
    clock_spec.tz = "Asia/Shanghai";

    RelationServiceBasis basis;
    basis.feature_base = "client_group_mix";
    basis.metric_name = "conn_count";
    basis.group_space_id = "client_group";
    basis.k_head = 5;
    basis.support_explicit = {11, 12, 13};
    basis.stable_head = {11};
    basis.head_proto_q = {1.0};

    EventCalendarSpec calendar_spec;
    calendar_spec.calendar_id = "relation-calendar";
    calendar_spec.calendar_version = "v1";
    calendar_spec.entries = {
        EventCalendarEntry{"global_event",
                           "global",
                           "local_wall_clock",
                           1711900800,
                           1711987199,
                           true,
                           "",
                           "",
                           "Asia/Shanghai"},
        EventCalendarEntry{"feature_event",
                           "feature",
                           "local_wall_clock",
                           1711900800,
                           1711987199,
                           true,
                           "client_group_mix_conn_count_entropy_shannon",
                           "",
                           "Asia/Shanghai"},
        EventCalendarEntry{"other_feature_event",
                           "feature",
                           "local_wall_clock",
                           1711900800,
                           1711987199,
                           true,
                           "client_group_mix_conn_count_top1_share",
                           "",
                           "Asia/Shanghai"},
    };

    TestRelationSourceResolver resolver;

    std::vector<RelationRoutedFeatureSpec> routed_specs;
    RelationRouter::BuildRoutedFeatureSpecs(spec,
                                           basis,
                                           clock_spec,
                                           BaselineStringRef{"svc-a", 5},
                                           &calendar_spec,
                                           &resolver,
                                           &routed_specs);
    assert(!routed_specs.empty());

    bool found_entropy = false;
    bool found_stable_g1 = false;
    bool found_stable_g2 = false;
    bool found_entropy_calendar = false;
    std::unordered_set<int32_t> local_slots;
    for (const auto& routed_spec : routed_specs) {
        assert(local_slots.insert(routed_spec.local_slot).second);
        if (routed_spec.feature ==
            "client_group_mix_conn_count_entropy_shannon") {
            found_entropy = true;
            assert(routed_spec.local_slot == 0);
            assert(routed_spec.delta == 60);
            assert(routed_spec.tz == "Asia/Shanghai");
            assert(routed_spec.baseline_source_config.has_value());
            assert(routed_spec.baseline_source_config->sources.size() == 1);
            assert(routed_spec.baseline_source_config->sources[0].source_key ==
                   "svc-source");
            assert(routed_spec.event_calendar_spec.has_value());
            assert(routed_spec.event_calendar_spec->entries.size() == 2);
            found_entropy_calendar = true;
        }
        if (routed_spec.feature ==
            "client_group_mix_conn_count_stable_g1_share") {
            found_stable_g1 = true;
        }
        if (routed_spec.feature ==
            "client_group_mix_conn_count_stable_g2_share") {
            found_stable_g2 = true;
        }
    }
    assert(found_entropy == true);
    assert(found_stable_g1 == true);
    assert(found_entropy_calendar == true);
    assert(found_stable_g2 == false);
    assert(resolver.call_count() == static_cast<int>(routed_specs.size()));
    assert(resolver.last_key() == "svc-a");

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
        if (routed_spec.feature ==
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

    TestRelationSourceResolver self_source_resolver(
        R"({"baseline_sources":[{"source_key":"svc-a"}]})");
    std::vector<RelationRoutedFeatureSpec> self_source_specs;
    RelationRouter::BuildRoutedFeatureSpecs(spec,
                                           basis,
                                           clock_spec,
                                           BaselineStringRef{"svc-a", 5},
                                           &calendar_spec,
                                           &self_source_resolver,
                                           &self_source_specs);
    assert(!self_source_specs.empty());
    for (const auto& routed_spec : self_source_specs) {
        assert(!routed_spec.baseline_source_config.has_value());
    }

    std::printf("[PASS] Relation routed feature mapping\n");
}

}  // namespace

int main() {
    TestProtocolContract();
    TestParseValueTaskBaselineSourceConfig();
    TestParseRelationTaskSpec();
    TestRelationEncodeTypeValidationOnSubmit();
    TestRelationOtherGroupIdxValidationOnSubmit();
    TestBuildRelationBasisAndEvalBasis();
    TestBuildRelationBasisExcludeOtherGroupIdxs();
    TestRelationTaskSeedMetricBasisViaTestAccess();
    TestKeyRiskFusionRemoveTaskContributionsByTaskId();
    TestExtractRelationSummaries();
    TestExtractRelationSummariesExcludeOtherGroupIdxs();
    TestRelationRouter();
    std::printf("[DONE] test_baseline_relation_task\n");
    return 0;
}
