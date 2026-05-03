/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

#include <common/loader.hpp>
#include <common/error_code.h>
#include <framework/interfaces/ibaseline_service.h>
#include <rapidjson/document.h>

#include "plugins/baseline/config/runtime_config.h"

using namespace flowsql;

namespace {

struct LoadedBaselineService {
    PluginLoader* loader = nullptr;
    IBaselineService* service = nullptr;

    ~LoadedBaselineService() {
        if (!loader) return;
        loader->StopAll();
        loader->Unload();
    }
};

LoadedBaselineService LoadBaselineService(const std::string& option = "") {
    LoadedBaselineService env;
    env.loader = PluginLoader::Single();

    std::string plugin_dir = get_absolute_process_path();
    std::string plugin_name = "libflowsql_baseline.so";
    const char* relapath[] = {plugin_name.c_str()};
    const char* option_value = option.empty() ? nullptr : option.c_str();
    const char* options[] = {option_value};

    const int ret = env.loader->Load(plugin_dir.c_str(), relapath, options, 1);
    assert(ret == 0);
    assert(env.loader->StartAll() == 0);

    env.service = static_cast<IBaselineService*>(env.loader->First(IID_BASELINE_SERVICE));
    assert(env.service != nullptr);
    return env;
}

const char* ValueTaskConfig() {
    return R"({
        "schema_version": 1,
        "task_id": "baseline_task_bps",
        "task_name": "link bps baseline",
        "task_kind": "value",
        "feature_id": "bps",
        "feature_type": "value_basic",
        "profile": "default",
        "clock_spec": {
            "bucket_seconds": 60,
            "timezone": "Asia/Shanghai"
        },
        "calendar_ref": {
            "calendar_id": "cn-holiday",
            "calendar_version": "2026.1"
        }
    })";
}

const char* SampledValueTaskConfig() {
    return R"({
        "schema_version": 1,
        "task_id": "baseline_task_sampled_bps",
        "task_name": "sampled link bps baseline",
        "task_kind": "value",
        "feature_id": "sampled_bps",
        "feature_type": "value_sampled",
        "profile": "cont_core",
        "clock_spec": {
            "bucket_seconds": 60,
            "timezone": "Asia/Shanghai"
        },
        "calendar_ref": {
            "calendar_id": "cn-holiday",
            "calendar_version": "2026.1"
        }
    })";
}

const char* OtherValueTaskConfig() {
    return R"({
        "schema_version": 1,
        "task_id": "baseline_task_other_bps",
        "task_name": "other link bps baseline",
        "task_kind": "value",
        "feature_id": "other_bps",
        "feature_type": "value_basic",
        "profile": "default",
        "clock_spec": {
            "bucket_seconds": 60,
            "timezone": "Asia/Shanghai"
        },
        "calendar_ref": {
            "calendar_id": "cn-holiday",
            "calendar_version": "2026.1"
        }
    })";
}

const char* LegacyValueTaskConfig() {
    return R"({
        "schema_version": 1,
        "task_id": "baseline_task_legacy_value",
        "task_name": "legacy value baseline",
        "task_kind": "value",
        "feature_id": "legacy_bps",
        "feature_type": "value",
        "profile": "default",
        "clock_spec": {
            "bucket_seconds": 60,
            "timezone": "Asia/Shanghai"
        },
        "calendar_ref": {
            "calendar_id": "cn-holiday",
            "calendar_version": "2026.1"
        }
    })";
}

const char* BasicValueWithSampledProfileConfig() {
    return R"({
        "schema_version": 1,
        "task_id": "baseline_task_basic_with_sampled_profile",
        "task_name": "invalid basic sampled profile",
        "task_kind": "value",
        "feature_id": "basic_bps",
        "feature_type": "value_basic",
        "profile": "cont_core",
        "clock_spec": {
            "bucket_seconds": 60,
            "timezone": "Asia/Shanghai"
        },
        "calendar_ref": {
            "calendar_id": "cn-holiday",
            "calendar_version": "2026.1"
        }
    })";
}

const char* SampledValueWithDefaultProfileConfig() {
    return R"({
        "schema_version": 1,
        "task_id": "baseline_task_sampled_with_default_profile",
        "task_name": "invalid sampled default profile",
        "task_kind": "value",
        "feature_id": "sampled_bps",
        "feature_type": "value_sampled",
        "profile": "default",
        "clock_spec": {
            "bucket_seconds": 60,
            "timezone": "Asia/Shanghai"
        },
        "calendar_ref": {
            "calendar_id": "cn-holiday",
            "calendar_version": "2026.1"
        }
    })";
}

const char* RatioTaskConfig() {
    return R"({
        "schema_version": 1,
        "task_id": "baseline_task_success_rate",
        "task_name": "success rate baseline",
        "task_kind": "ratio",
        "feature_id": "success_rate",
        "feature_type": "ratio",
        "profile": "rate_core",
        "clock_spec": {
            "bucket_seconds": 60,
            "timezone": "Asia/Shanghai"
        },
        "calendar_ref": {
            "calendar_id": "cn-holiday",
            "calendar_version": "2026.1"
        }
    })";
}

const char* RelationTaskConfig() {
    return R"({
        "schema_version": 1,
        "task_id": "baseline_task_client_mix",
        "task_name": "client mix basis",
        "task_kind": "relation",
        "feature_id": "client_mix",
        "feature_type": "relation",
        "profile": "default",
        "group_space_id": "client_group",
        "group_space_version": "v1",
        "metrics": ["bps"],
        "support_policy": {
            "k_support": 2,
            "min_hist_share": 0.01,
            "min_active_ratio": 0.1
        },
        "summary_policy": {
            "k_head": 2,
            "k_stable": 1
        },
        "clock_spec": {
            "bucket_seconds": 60,
            "timezone": "Asia/Shanghai"
        },
        "calendar_ref": {
            "calendar_id": "cn-holiday",
            "calendar_version": "2026.1"
        }
    })";
}

void TestEventCalendarSchemaRejectsTaskScopedFields() {
    std::printf("[TEST] B1 calendar schema rejects task-scoped fields...\n");

    const std::string config_path = "/tmp/flowsql_baseline_calendar_schema_test.yaml";
    {
        std::ofstream file(config_path);
        file << R"(
calendars:
  - calendar_id: "cn-holiday"
    calendar_version: "2026.1"
    entries:
      - event_code: "promo"
        scope_type: "feature"
        feature: "bps"
        alignment_mode: "absolute_utc"
        start_ts: 1200
        end_ts: 1500
baseline:
  parser:
    tz_default: "UTC"
  shared_profile_config:
    daily_harmonic_order: 2
    weekly_harmonic_order: 1
    dme_max: 7
    m_month_enable: 4
    month_cov_min: 0.8
    lambda_season: 1.0
    lambda_dom: 4.0
    lambda_dme: 2.0
    lambda_lwd: 1.0
    lambda_event: 0.1
  value_sampled_profiles:
    cont_core:
      n_train_min: 50
      transform_name_override: "log1p"
  ratio_profiles:
    global:
      eps_logit: 1.0e-4
      m_floor: 1.0e-4
      v_floor: 0.25
    rate_core:
      d_min_train: 50
      s_prior: 2.0
      phi_over: 1.5
  solver_constants:
    solver_name: "weighted_huber_ridge_irls"
    c_huber: 1.5
    s_min_fit: 1.0e-3
    max_iter_fit: 15
    tol_obj_rel: 1.0e-4
    tol_beta_inf: 1.0e-5
    cond_max: 1.0e8
)";
    }

    std::string err;
    const int rc = flowsql::baseline::LoadBaselineRuntimeConfigFromYaml(config_path, true, &err);
    assert(rc == error::BAD_REQUEST);
    assert(err.find("not allowed") != std::string::npos || err.find("feature") != std::string::npos);
    flowsql::baseline::ResetBaselineRuntimeConfig();

    std::printf("[PASS] B1 calendar schema rejects task-scoped fields\n");
}

void AssertSnapshotHasTask(const std::string& json,
                           const char* expected_task_id,
                           uint64_t expected_count) {
    rapidjson::Document doc;
    doc.Parse(json.c_str());
    assert(!doc.HasParseError());
    assert(doc.IsObject());
    assert(doc.HasMember("task_count"));
    assert(doc["task_count"].GetUint64() == expected_count);
    assert(doc.HasMember("tasks"));
    assert(doc["tasks"].IsArray());

    bool found = false;
    for (const auto& item : doc["tasks"].GetArray()) {
        assert(item.IsObject());
        assert(item.HasMember("task_id"));
        if (std::string(item["task_id"].GetString()) == expected_task_id) {
            found = true;
            assert(item.HasMember("kind"));
            assert(std::string(item["kind"].GetString()) == "value");
        }
    }
    assert(found == (expected_count > 0));
}

void TestCreateTaskUsesConfigIdentity() {
    std::printf("[TEST] B1 task config identity...\n");
    auto env = LoadBaselineService();

    auto [status, task] = env.service->CreateValueTask(
        ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(task != nullptr);
    assert(std::string(task->Id()) == "baseline_task_bps");
    assert(std::string(task->Name()) == "link bps baseline");

    auto [snapshot_status, snapshot] =
        env.service->QueryServiceSnapshot(BaselineSerializationFormat::kJson);
    assert(snapshot_status == BaselineStatus::kOk);
    AssertSnapshotHasTask(snapshot, "baseline_task_bps", 1);

    auto [dup_status, dup_task] = env.service->CreateValueTask(
        ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(dup_status == BaselineStatus::kInvalidArgument);
    assert(dup_task == nullptr);

    assert(task->Close() == BaselineStatus::kOk);
    auto [closed_snapshot_status, closed_snapshot] =
        env.service->QueryServiceSnapshot(BaselineSerializationFormat::kJson);
    assert(closed_snapshot_status == BaselineStatus::kOk);
    AssertSnapshotHasTask(closed_snapshot, "baseline_task_bps", 0);

    std::printf("[PASS] B1 task config identity\n");
}

void TestInvalidTaskConfigRejected() {
    std::printf("[TEST] B1 invalid task config rejected...\n");
    auto env = LoadBaselineService();

    auto [status, task] = env.service->CreateValueTask(
        R"({"schema_version":1})", BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kParseFailed);
    assert(task == nullptr);

    auto [legacy_status, legacy_task] = env.service->CreateValueTask(
        LegacyValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(legacy_status == BaselineStatus::kParseFailed);
    assert(legacy_task == nullptr);

    auto [basic_sampled_status, basic_sampled_task] = env.service->CreateValueTask(
        BasicValueWithSampledProfileConfig(), BaselineSerializationFormat::kJson);
    assert(basic_sampled_status == BaselineStatus::kParseFailed);
    assert(basic_sampled_task == nullptr);

    auto [sampled_default_status, sampled_default_task] = env.service->CreateValueTask(
        SampledValueWithDefaultProfileConfig(), BaselineSerializationFormat::kJson);
    assert(sampled_default_status == BaselineStatus::kParseFailed);
    assert(sampled_default_task == nullptr);

    std::printf("[PASS] B1 invalid task config rejected\n");
}

ValueBootstrapInput BuildValueHistoryForSeries(const std::string& series_key,
                                               double base_value) {
    ValueBootstrapInput input;
    input.series_key = series_key;
    for (int64_t bucket = 0; bucket < 200; ++bucket) {
        input.observations.push_back(
            ValueBootstrapPoint{bucket, base_value + static_cast<double>(bucket % 10), 1});
    }
    return input;
}

ValueBootstrapInput BuildValueHistory() {
    return BuildValueHistoryForSeries("svc-a", 100.0);
}

ValueBootstrapInput BuildEventValueHistory() {
    ValueBootstrapInput input;
    input.series_key = "svc-event";
    for (int64_t bucket = 0; bucket < 200; ++bucket) {
        const bool event_bucket =
            (bucket >= 20 && bucket < 25) ||
            (bucket >= 80 && bucket < 85) ||
            (bucket >= 140 && bucket < 145);
        const double value = 100.0 + static_cast<double>(bucket % 3) +
                             (event_bucket ? 400.0 : 0.0);
        input.observations.push_back(ValueBootstrapPoint{bucket, value, 1});
    }
    return input;
}

RatioBootstrapInput BuildRatioHistory() {
    RatioBootstrapInput input;
    input.series_key = "svc-a";
    for (int64_t bucket = 0; bucket < 200; ++bucket) {
        input.observations.push_back(
            RatioBootstrapPoint{bucket, 95.0 + static_cast<double>(bucket % 3), 100.0});
    }
    return input;
}

ValueBootstrapInput BuildSampledValueHistory() {
    ValueBootstrapInput input = BuildValueHistory();
    for (auto& point : input.observations) {
        point.sample_count = 50;
    }
    return input;
}

RelationBootstrapInput BuildRelationHistory() {
    RelationBootstrapInput input;
    input.series_key = "svc-a";
    for (int64_t bucket = 0; bucket < 10; ++bucket) {
        RelationBootstrapBlock block;
        block.bucket_id = bucket;
        block.group_idx = {1, 2, 3};
        RelationBootstrapMetric metric;
        metric.metric = "bps";
        metric.total = 100.0;
        metric.values_by_group = {60.0, 30.0, 10.0};
        block.metrics.push_back(metric);
        input.blocks.push_back(block);
    }
    return input;
}

RelationRollingObservation BuildRelationObservation(int64_t bucket,
                                                    double g1,
                                                    double g2,
                                                    double g3) {
    RelationRollingObservation obs;
    obs.series_key = "svc-a";
    obs.bucket_id = bucket;
    obs.group_idx = {1, 2, 3};
    RelationBootstrapMetric metric;
    metric.metric = "bps";
    metric.total = g1 + g2 + g3;
    metric.active_count = 3;
    metric.values_by_group = {g1, g2, g3};
    obs.metrics.push_back(metric);
    return obs;
}

void TestTaskBootstrapPredictAndExport() {
    std::printf("[TEST] B1 task bootstrap predict export...\n");
    auto env = LoadBaselineService();

    auto [value_status, value_task] = env.service->CreateValueTask(
        ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(value_status == BaselineStatus::kOk);
    assert(value_task != nullptr);
    const BootstrapTrainResult value_train = value_task->Bootstrap(BuildValueHistory());
    assert(value_train.status == BaselineStatus::kOk);
    const BootstrapPrediction value_prediction =
        value_task->PredictBootstrap("svc-a", 220, BootstrapPredictionOptions{});
    assert(value_prediction.status == BaselineStatus::kOk);
    auto [value_artifact_status, value_artifact] =
        value_task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);
    assert(value_artifact_status == BaselineStatus::kOk);
    assert(value_artifact.find("\"document_kind\":\"bootstrap_artifact\"") != std::string::npos);
    auto [value_seed_status, value_seed] =
        value_task->ExportBootstrapSeed(BaselineSerializationFormat::kJson);
    assert(value_seed_status == BaselineStatus::kOk);
    assert(value_seed.find("\"document_kind\":\"bootstrap_seed\"") != std::string::npos);
    assert(value_seed.find("\"algorithm_version\":\"b1-bootstrap-v1\"") != std::string::npos);
    assert(value_seed.find("\"feature_type\":\"value_basic\"") != std::string::npos);
    assert(value_seed.find("\"calendar_ref\"") != std::string::npos);
    assert(value_seed.find("\"calendar_id\":\"cn-holiday\"") != std::string::npos);
    assert(value_seed.find("\"calendar_version\":\"2026.1\"") != std::string::npos);
    assert(value_seed.find("\"clock_spec\"") != std::string::npos);
    assert(value_seed.find("\"bucket_seconds\":60") != std::string::npos);
    assert(value_seed.find("\"timezone\":\"Asia/Shanghai\"") != std::string::npos);
    assert(value_seed.find("\"seeded_components\"") != std::string::npos);
    assert(value_seed.find("\"enabled_components\"") != std::string::npos);
    assert(value_seed.find("\"level\"") != std::string::npos);
    assert(value_seed.find("\"trend\"") != std::string::npos);
    assert(value_seed.find("\"daily\"") != std::string::npos);
    assert(value_seed.find("\"weekly\"") != std::string::npos);
    assert(value_seed.find("\"core\"") == std::string::npos);
    assert(value_seed.find("\"theta_init\"") != std::string::npos);
    assert(value_seed.find("\"sigma_init\"") != std::string::npos);
    assert(value_seed.find("\"uncertainty_init\"") != std::string::npos);
    assert(value_seed.find("\"component_uncertainty\"") != std::string::npos);
    assert(value_seed.find("\"maturity_init\"") != std::string::npos);
    assert(value_seed.find("\"model\"") == std::string::npos);

    auto [sampled_value_status, sampled_value_task] = env.service->CreateValueTask(
        SampledValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(sampled_value_status == BaselineStatus::kOk);
    assert(sampled_value_task != nullptr);
    const BootstrapTrainResult sampled_value_train =
        sampled_value_task->Bootstrap(BuildSampledValueHistory());
    assert(sampled_value_train.status == BaselineStatus::kOk);

    auto [ratio_status, ratio_task] = env.service->CreateRatioTask(
        RatioTaskConfig(), BaselineSerializationFormat::kJson);
    assert(ratio_status == BaselineStatus::kOk);
    assert(ratio_task != nullptr);
    const BootstrapTrainResult ratio_train = ratio_task->Bootstrap(BuildRatioHistory());
    assert(ratio_train.status == BaselineStatus::kOk);
    const BootstrapPrediction ratio_prediction =
        ratio_task->PredictBootstrap("svc-a", 220, BootstrapPredictionOptions{});
    assert(ratio_prediction.status == BaselineStatus::kOk);
    assert(ratio_prediction.baseline_mu >= 0.0);
    assert(ratio_prediction.baseline_mu <= 1.0);
    auto [ratio_seed_status, ratio_seed] =
        ratio_task->ExportBootstrapSeed(BaselineSerializationFormat::kJson);
    assert(ratio_seed_status == BaselineStatus::kOk);
    assert(ratio_seed.find("\"feature_type\":\"ratio\"") != std::string::npos);
    assert(ratio_seed.find("\"calendar_ref\"") != std::string::npos);
    assert(ratio_seed.find("\"clock_spec\"") != std::string::npos);
    assert(ratio_seed.find("\"seeded_components\"") != std::string::npos);
    assert(ratio_seed.find("\"enabled_components\"") != std::string::npos);
    assert(ratio_seed.find("\"theta_init\"") != std::string::npos);
    assert(ratio_seed.find("\"sigma_init\"") != std::string::npos);
    assert(ratio_seed.find("\"uncertainty_init\"") != std::string::npos);
    assert(ratio_seed.find("\"component_uncertainty\"") != std::string::npos);
    assert(ratio_seed.find("\"maturity_init\"") != std::string::npos);
    assert(ratio_seed.find("\"ratio_prior_init\"") != std::string::npos);
    assert(ratio_seed.find("\"model\"") == std::string::npos);

    auto [relation_status, relation_task] = env.service->CreateRelationTask(
        RelationTaskConfig(), BaselineSerializationFormat::kJson);
    assert(relation_status == BaselineStatus::kOk);
    assert(relation_task != nullptr);
    const BootstrapTrainResult relation_train =
        relation_task->Bootstrap(BuildRelationHistory());
    assert(relation_train.status == BaselineStatus::kOk);
    auto [relation_snapshot_status, relation_snapshot] =
        relation_task->QueryTaskSnapshot(BaselineSerializationFormat::kJson);
    assert(relation_snapshot_status == BaselineStatus::kOk);
    rapidjson::Document relation_snapshot_doc;
    relation_snapshot_doc.Parse(relation_snapshot.c_str());
    assert(!relation_snapshot_doc.HasParseError());
    assert(relation_snapshot_doc.HasMember("document_kind"));
    assert(std::string(relation_snapshot_doc["document_kind"].GetString()) ==
           "relation_task_snapshot");
    assert(relation_snapshot_doc.HasMember("relation_runtime"));
    const auto& relation_runtime = relation_snapshot_doc["relation_runtime"];
    assert(relation_runtime["routed_shard_count"].GetUint64() == 16);
    assert(relation_runtime["source_state_count"].GetUint64() == 1);
    assert(relation_runtime["routed_seed_count"].GetUint64() > 0);
    auto [basis_status, basis_json] =
        relation_task->QueryBootstrapBasis(BaselineSerializationFormat::kJson);
    assert(basis_status == BaselineStatus::kOk);
    assert(basis_json.find("\"support_explicit\"") != std::string::npos);
    auto [relation_seed_status, relation_seed] =
        relation_task->ExportBootstrapSeed(BaselineSerializationFormat::kJson);
    assert(relation_seed_status == BaselineStatus::kOk);
    assert(relation_seed.find("\"feature_type\":\"relation\"") != std::string::npos);
    assert(relation_seed.find("\"calendar_ref\"") != std::string::npos);
    assert(relation_seed.find("\"feature_type\":\"value_basic\"") != std::string::npos);
    assert(relation_seed.find("\"feature_type\":\"ratio\"") != std::string::npos);
    assert(relation_seed.find("\"clock_spec\"") != std::string::npos);
    assert(relation_seed.find("\"seeded_components\"") != std::string::npos);
    assert(relation_seed.find("\"enabled_components\"") != std::string::npos);
    assert(relation_seed.find("\"relation_basis\"") != std::string::npos);
    assert(relation_seed.find("\"relation_routed_summary_seeds\"") != std::string::npos);
    assert(relation_seed.find("\"relation_basis_by_metric\"") != std::string::npos);
    assert(relation_seed.find("\"summary\":\"entropy_shannon\"") != std::string::npos);
    assert(relation_seed.find("\"summary\":\"top1_share\"") != std::string::npos);
    assert(relation_seed.find("\"theta_init\"") != std::string::npos);
    assert(relation_seed.find("\"sigma_init\"") != std::string::npos);
    assert(relation_seed.find("\"uncertainty_init\"") != std::string::npos);
    assert(relation_seed.find("\"component_uncertainty\"") != std::string::npos);
    assert(relation_seed.find("\"maturity_init\"") != std::string::npos);
    assert(relation_seed.find("\"ratio_prior_init\"") != std::string::npos);
    assert(relation_seed.find("\"model\"") == std::string::npos);

    std::printf("[PASS] B1 task bootstrap predict export\n");
}

void TestValueTaskKeepsBootstrapPerSeriesAndExportsAll() {
    std::printf("[TEST] B1 value task keeps bootstrap per series and exports all...\n");

    auto env = LoadBaselineService();
    auto [value_status, value_task] = env.service->CreateValueTask(
        ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(value_status == BaselineStatus::kOk);
    assert(value_task != nullptr);

    const BootstrapTrainResult train_a =
        value_task->Bootstrap(BuildValueHistoryForSeries("svc-a", 100.0));
    assert(train_a.status == BaselineStatus::kOk);
    ValueBootstrapInput no_replace_input = BuildValueHistoryForSeries("svc-a", 120.0);
    no_replace_input.options.force_replace_existing_artifact = false;
    const BootstrapTrainResult no_replace_result =
        value_task->Bootstrap(no_replace_input);
    assert(no_replace_result.status == BaselineStatus::kInvalidArgument);
    const BootstrapTrainResult train_b =
        value_task->Bootstrap(BuildValueHistoryForSeries("svc-b", 500.0));
    assert(train_b.status == BaselineStatus::kOk);

    const BootstrapPrediction prediction_a =
        value_task->PredictBootstrap("svc-a", 220, BootstrapPredictionOptions{});
    const BootstrapPrediction prediction_b =
        value_task->PredictBootstrap("svc-b", 220, BootstrapPredictionOptions{});
    assert(prediction_a.status == BaselineStatus::kOk);
    assert(prediction_b.status == BaselineStatus::kOk);
    assert(prediction_a.series_key == "svc-a");
    assert(prediction_b.series_key == "svc-b");
    assert(prediction_b.baseline_mu > prediction_a.baseline_mu + 300.0);

    auto [artifact_status, artifact_json] =
        value_task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);
    assert(artifact_status == BaselineStatus::kOk);
    assert(artifact_json.find("\"series_artifacts\"") != std::string::npos);
    assert(artifact_json.find("\"series_key\":\"svc-a\"") != std::string::npos);
    assert(artifact_json.find("\"series_key\":\"svc-b\"") != std::string::npos);

    auto [seed_status, seed_json] =
        value_task->ExportBootstrapSeed(BaselineSerializationFormat::kJson);
    assert(seed_status == BaselineStatus::kOk);
    assert(seed_json.find("\"series_seeds\"") != std::string::npos);
    assert(seed_json.find("\"series_key\":\"svc-a\"") != std::string::npos);
    assert(seed_json.find("\"series_key\":\"svc-b\"") != std::string::npos);

    auto [reload_status, reload_task] = env.service->CreateValueTask(
        R"({
            "schema_version": 1,
            "task_id": "baseline_task_bps_reload",
            "task_name": "link bps baseline reload",
            "task_kind": "value",
            "feature_id": "bps",
            "feature_type": "value_basic",
            "profile": "default",
            "clock_spec": {
                "bucket_seconds": 60,
                "timezone": "Asia/Shanghai"
            },
            "calendar_ref": {
                "calendar_id": "cn-holiday",
                "calendar_version": "2026.1"
            }
        })",
        BaselineSerializationFormat::kJson);
    assert(reload_status == BaselineStatus::kOk);
    assert(reload_task != nullptr);
    assert(reload_task->LoadBootstrapArtifact(
               artifact_json, BaselineSerializationFormat::kJson) ==
           BaselineStatus::kIncompatibleArtifact);

    assert(value_task->Close() == BaselineStatus::kOk);
    auto [restored_status, restored_task] = env.service->CreateValueTask(
        ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(restored_status == BaselineStatus::kOk);
    assert(restored_task != nullptr);
    assert(restored_task->LoadBootstrapArtifact(
               artifact_json, BaselineSerializationFormat::kJson) == BaselineStatus::kOk);
    const BootstrapPrediction reloaded_prediction_a =
        restored_task->PredictBootstrap("svc-a", 220, BootstrapPredictionOptions{});
    const BootstrapPrediction reloaded_prediction_b =
        restored_task->PredictBootstrap("svc-b", 220, BootstrapPredictionOptions{});
    assert(reloaded_prediction_a.status == BaselineStatus::kOk);
    assert(reloaded_prediction_b.status == BaselineStatus::kOk);
    assert(reloaded_prediction_b.baseline_mu > reloaded_prediction_a.baseline_mu + 300.0);

    std::printf("[PASS] B1 value task keeps bootstrap per series and exports all\n");
}

void TestB4RelationSubmitObservation() {
    std::printf("[TEST] B4 relation submit observation routes summaries...\n");

    auto env = LoadBaselineService();
    auto [status, relation_task] = env.service->CreateRelationTask(
        RelationTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(relation_task != nullptr);

    RelationRollingSubmitOptions options;
    RelationRollingResult cold_result =
        relation_task->SubmitObservation(BuildRelationObservation(100, 60, 30, 10), options);
    assert(cold_result.status == BaselineStatus::kOk);
    assert(cold_result.series_key == "svc-a");
    assert(cold_result.bucket_id == 100);
    assert(cold_result.basis_status == "collecting");

    bool has_entropy = false;
    bool has_top1 = false;
    bool has_basis_scoped = false;
    std::string top1_routed_series_key;
    for (const auto& routed : cold_result.routed_results) {
        if (routed.summary == "entropy_shannon") {
            has_entropy = true;
            assert(!routed.basis_scoped);
            assert(routed.rolling.status == BaselineStatus::kOk);
        }
        if (routed.summary == "top1_share") {
            has_top1 = true;
            assert(!routed.basis_scoped);
            assert(routed.rolling.status == BaselineStatus::kOk);
            top1_routed_series_key = routed.routed_series_key;
        }
        if (routed.basis_scoped) has_basis_scoped = true;
    }
    assert(has_entropy);
    assert(has_top1);
    assert(!has_basis_scoped);
    assert(!top1_routed_series_key.empty());

    auto [snapshot_status, snapshot] =
        relation_task->QueryTaskSnapshot(BaselineSerializationFormat::kJson);
    assert(snapshot_status == BaselineStatus::kOk);
    rapidjson::Document snapshot_doc;
    snapshot_doc.Parse(snapshot.c_str());
    assert(!snapshot_doc.HasParseError());
    assert(snapshot_doc["relation_runtime"]["source_state_count"].GetUint64() == 1);
    assert(snapshot_doc["relation_runtime"]["routed_state_count"].GetUint64() > 0);

    auto [source_snapshot_status, source_snapshot] =
        relation_task->QuerySeriesSnapshot("svc-a", BaselineSerializationFormat::kJson);
    assert(source_snapshot_status == BaselineStatus::kOk);
    rapidjson::Document source_snapshot_doc;
    source_snapshot_doc.Parse(source_snapshot.c_str());
    assert(!source_snapshot_doc.HasParseError());
    assert(std::string(source_snapshot_doc["document_kind"].GetString()) ==
           "relation_series_snapshot");
    assert(source_snapshot_doc["series_key"].GetString() == std::string("svc-a"));
    assert(source_snapshot_doc["source_series_key"].GetString() == std::string("svc-a"));
    assert(source_snapshot_doc.HasMember("basis_by_metric"));
    assert(source_snapshot_doc["basis_by_metric"].IsArray());
    assert(source_snapshot_doc.HasMember("routed_summaries"));
    assert(source_snapshot_doc["routed_summaries"].IsArray());
    assert(source_snapshot_doc["routed_summaries"].Size() > 0);

    auto [wrong_source_status, wrong_source_snapshot] =
        relation_task->QuerySeriesSnapshot(top1_routed_series_key,
                                           BaselineSerializationFormat::kJson);
    assert(wrong_source_status == BaselineStatus::kInvalidArgument);
    assert(wrong_source_snapshot.empty());

    RelationRoutedSummaryQuery routed_query;
    routed_query.source_series_key = "svc-a";
    routed_query.metric = "bps";
    routed_query.summary = "top1_share";
    routed_query.feature_type = "ratio";
    RollingPrediction routed_prediction =
        relation_task->PredictRoutedSummary(routed_query, 101);
    assert(routed_prediction.status == BaselineStatus::kOk);

    auto [routed_snapshot_status, routed_snapshot] =
        relation_task->QueryRoutedSummarySnapshot(routed_query,
                                                  BaselineSerializationFormat::kJson);
    assert(routed_snapshot_status == BaselineStatus::kOk);
    rapidjson::Document routed_snapshot_doc;
    routed_snapshot_doc.Parse(routed_snapshot.c_str());
    assert(!routed_snapshot_doc.HasParseError());
    assert(std::string(routed_snapshot_doc["document_kind"].GetString()) ==
           "rolling_series_snapshot");
    assert(routed_snapshot_doc["last_seen_bucket"].GetInt64() == 100);

    std::string seeded_config = RelationTaskConfig();
    const std::string old_task_id = "baseline_task_client_mix";
    const std::size_t task_id_pos = seeded_config.find(old_task_id);
    assert(task_id_pos != std::string::npos);
    seeded_config.replace(task_id_pos, old_task_id.size(), "baseline_task_client_mix_seeded");
    auto [seeded_status, seeded_task] = env.service->CreateRelationTask(
        seeded_config, BaselineSerializationFormat::kJson);
    assert(seeded_status == BaselineStatus::kOk);
    assert(seeded_task != nullptr);
    assert(seeded_task->Bootstrap(BuildRelationHistory()).status == BaselineStatus::kOk);
    RelationRollingResult seeded_result =
        seeded_task->SubmitObservation(BuildRelationObservation(20, 50, 25, 25), options);
    assert(seeded_result.status == BaselineStatus::kOk);

    bool has_out_of_support = false;
    for (const auto& routed : seeded_result.routed_results) {
        if (routed.summary == "out_of_support_share") {
            has_out_of_support = true;
            assert(routed.basis_scoped);
            assert(routed.basis_version > 0);
            assert(routed.rolling.status == BaselineStatus::kOk);
        }
    }
    assert(has_out_of_support);

    std::printf("[PASS] B4 relation submit observation routes summaries\n");
}

void TestB4RelationRollingConfigSwitches() {
    std::printf("[TEST] B4 relation rolling config switches close design contract...\n");

    const std::string routed_disabled_config = "/tmp/flowsql_b4_relation_routed_disabled.yaml";
    {
        std::ofstream file(routed_disabled_config);
        file << R"(
baseline:
  rolling_config:
    relation_rolling:
      enable_routed_rolling: false
      enable_stream_basis: true
      include_universal_summaries_without_basis: true
      basis_collect_min_buckets: 1
      basis_ready_min_buckets: 1
      basis_refresh_interval_buckets: 1
      basis_candidate_min_coverage_ratio: 0.01
      basis_min_stable_refresh_count: 1
      routed_state_shard_count: 4
)";
        assert(file.good());
    }
    {
        auto env = LoadBaselineService("config_file=" + routed_disabled_config + ";strict=false");
        auto [status, relation_task] = env.service->CreateRelationTask(
            RelationTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        assert(relation_task != nullptr);

        RelationRollingSubmitOptions options;
        RelationRollingResult result =
            relation_task->SubmitObservation(BuildRelationObservation(100, 60, 30, 10), options);
        assert(result.status == BaselineStatus::kOk);
        assert(result.routed_results.empty());
        assert(result.basis_version == 1);

        auto [task_snapshot_status, task_snapshot] =
            relation_task->QueryTaskSnapshot(BaselineSerializationFormat::kJson);
        assert(task_snapshot_status == BaselineStatus::kOk);
        rapidjson::Document task_doc;
        task_doc.Parse(task_snapshot.c_str());
        assert(!task_doc.HasParseError());
        assert(task_doc["relation_runtime"]["routed_shard_count"].GetUint64() == 4);
        assert(task_doc["relation_runtime"]["routed_state_count"].GetUint64() == 0);

        auto [source_status, source_snapshot] =
            relation_task->QuerySeriesSnapshot("svc-a", BaselineSerializationFormat::kJson);
        assert(source_status == BaselineStatus::kOk);
        rapidjson::Document source_doc;
        source_doc.Parse(source_snapshot.c_str());
        assert(!source_doc.HasParseError());
        assert(source_doc["basis_by_metric"].IsArray());
        assert(source_doc["basis_by_metric"].Size() == 1);
        assert(source_doc["routed_summaries"].IsArray());
        assert(source_doc["routed_summaries"].Empty());
    }
    flowsql::baseline::ResetBaselineRuntimeConfig();

    const std::string stream_disabled_config = "/tmp/flowsql_b4_relation_stream_disabled.yaml";
    {
        std::ofstream file(stream_disabled_config);
        file << R"(
baseline:
  rolling_config:
    relation_rolling:
      enable_routed_rolling: true
      enable_stream_basis: false
      include_universal_summaries_without_basis: true
      routed_state_shard_count: 4
)";
        assert(file.good());
    }
    {
        auto env = LoadBaselineService("config_file=" + stream_disabled_config + ";strict=false");
        auto [status, relation_task] = env.service->CreateRelationTask(
            RelationTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        assert(relation_task != nullptr);

        RelationRollingSubmitOptions options;
        RelationRollingResult result =
            relation_task->SubmitObservation(BuildRelationObservation(100, 60, 30, 10), options);
        assert(result.status == BaselineStatus::kOk);
        assert(!result.routed_results.empty());

        auto [source_status, source_snapshot] =
            relation_task->QuerySeriesSnapshot("svc-a", BaselineSerializationFormat::kJson);
        assert(source_status == BaselineStatus::kOk);
        rapidjson::Document source_doc;
        source_doc.Parse(source_snapshot.c_str());
        assert(!source_doc.HasParseError());
        assert(source_doc["basis_by_metric"].IsArray());
        assert(source_doc["basis_by_metric"].Empty());
        assert(source_doc["routed_summaries"].IsArray());
        assert(source_doc["routed_summaries"].Size() > 0);
    }
    flowsql::baseline::ResetBaselineRuntimeConfig();

    const std::string universal_disabled_config =
        "/tmp/flowsql_b4_relation_universal_disabled.yaml";
    {
        std::ofstream file(universal_disabled_config);
        file << R"(
baseline:
  rolling_config:
    relation_rolling:
      enable_routed_rolling: true
      enable_stream_basis: false
      include_universal_summaries_without_basis: false
      routed_state_shard_count: 4
)";
        assert(file.good());
    }
    {
        auto env = LoadBaselineService("config_file=" + universal_disabled_config + ";strict=false");
        auto [status, relation_task] = env.service->CreateRelationTask(
            RelationTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        assert(relation_task != nullptr);

        RelationRollingSubmitOptions options;
        RelationRollingResult result =
            relation_task->SubmitObservation(BuildRelationObservation(100, 60, 30, 10), options);
        assert(result.status == BaselineStatus::kOk);
        assert(result.routed_results.empty());

        auto [source_status, source_snapshot] =
            relation_task->QuerySeriesSnapshot("svc-a", BaselineSerializationFormat::kJson);
        assert(source_status == BaselineStatus::kNotTrained);
        assert(source_snapshot.empty());
    }
    flowsql::baseline::ResetBaselineRuntimeConfig();

    std::printf("[PASS] B4 relation rolling config switches close design contract\n");
}

void TestBootstrapUsesConfiguredEventCalendar() {
    std::printf("[TEST] B1 bootstrap uses configured event calendar...\n");

    const std::string config_path = "/tmp/flowsql_baseline_event_calendar_test.yaml";
    {
        std::ofstream file(config_path);
        file << R"(
calendars:
  - calendar_id: "cn-holiday"
    calendar_version: "2026.1"
    entries:
      - event_code: "holiday"
        alignment_mode: "absolute_utc"
        start_ts: 1200
        end_ts: 1500
      - event_code: "holiday"
        alignment_mode: "absolute_utc"
        start_ts: 4800
        end_ts: 5100
      - event_code: "holiday"
        alignment_mode: "absolute_utc"
        start_ts: 8400
        end_ts: 8700
      - event_code: "holiday"
        alignment_mode: "absolute_utc"
        start_ts: 13200
        end_ts: 13500
baseline:
  parser:
    tz_default: "UTC"
  shared_profile_config:
    daily_harmonic_order: 2
    weekly_harmonic_order: 1
    dme_max: 7
    m_month_enable: 4
    month_cov_min: 0.8
    lambda_season: 1.0
    lambda_dom: 4.0
    lambda_dme: 2.0
    lambda_lwd: 1.0
    lambda_event: 0.1
  value_sampled_profiles:
    cont_core:
      n_train_min: 50
      transform_name_override: "log1p"
  ratio_profiles:
    global:
      eps_logit: 1.0e-4
      m_floor: 1.0e-4
      v_floor: 0.25
    rate_core:
      d_min_train: 50
      s_prior: 2.0
      phi_over: 1.5
  solver_constants:
    solver_name: "weighted_huber_ridge_irls"
    c_huber: 1.5
    s_min_fit: 1.0e-3
    max_iter_fit: 15
    tol_obj_rel: 1.0e-4
    tol_beta_inf: 1.0e-5
    cond_max: 1.0e8
)";
        assert(file.good());
    }

    auto env = LoadBaselineService("config_file=" + config_path + ";strict=true");
    auto [value_status, value_task] = env.service->CreateValueTask(
        ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(value_status == BaselineStatus::kOk);
    assert(value_task != nullptr);

    const BootstrapTrainResult train = value_task->Bootstrap(BuildEventValueHistory());
    assert(train.status == BaselineStatus::kOk);

    auto [artifact_status, artifact_json] =
        value_task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);
    assert(artifact_status == BaselineStatus::kOk);
    assert(artifact_json.find("\"event_block\"") != std::string::npos);
    assert(artifact_json.find("\"active_event_codes\"") != std::string::npos);
    assert(artifact_json.find("\"holiday\"") != std::string::npos);

    auto [seed_status, seed_json] =
        value_task->ExportBootstrapSeed(BaselineSerializationFormat::kJson);
    assert(seed_status == BaselineStatus::kOk);
    assert(seed_json.find("\"event_hint\"") != std::string::npos);
    assert(seed_json.find("\"event\"") != std::string::npos);
    assert(seed_json.find("\"holiday\"") != std::string::npos);

    const BootstrapPrediction event_prediction =
        value_task->PredictBootstrap("svc-event", 220, BootstrapPredictionOptions{});
    const BootstrapPrediction normal_prediction =
        value_task->PredictBootstrap("svc-event", 230, BootstrapPredictionOptions{});
    assert(event_prediction.status == BaselineStatus::kOk);
    assert(normal_prediction.status == BaselineStatus::kOk);
    assert(event_prediction.baseline_mu > normal_prediction.baseline_mu + 100.0);

    std::printf("[PASS] B1 bootstrap uses configured event calendar\n");
}

void TestTaskRejectsIncompatibleBootstrapArtifact() {
    std::printf("[TEST] B1 task rejects incompatible bootstrap artifact...\n");

    auto env = LoadBaselineService();
    auto [value_status, value_task] = env.service->CreateValueTask(
        ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(value_status == BaselineStatus::kOk);
    assert(value_task != nullptr);
    const BootstrapTrainResult train = value_task->Bootstrap(BuildValueHistory());
    assert(train.status == BaselineStatus::kOk);
    auto [artifact_status, artifact_json] =
        value_task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);
    assert(artifact_status == BaselineStatus::kOk);
    assert(value_task->LoadBootstrapArtifact(
               artifact_json, BaselineSerializationFormat::kJson) == BaselineStatus::kOk);

    auto [other_status, other_task] = env.service->CreateValueTask(
        OtherValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(other_status == BaselineStatus::kOk);
    assert(other_task != nullptr);
    assert(other_task->LoadBootstrapArtifact(
               artifact_json, BaselineSerializationFormat::kJson) ==
           BaselineStatus::kIncompatibleArtifact);

    std::string bad_schema_json = artifact_json;
    const std::string schema_needle = "\"schema_version\":1";
    const std::size_t schema_pos = bad_schema_json.find(schema_needle);
    assert(schema_pos != std::string::npos);
    bad_schema_json.replace(schema_pos, schema_needle.size(), "\"schema_version\":2");
    assert(value_task->LoadBootstrapArtifact(
               bad_schema_json, BaselineSerializationFormat::kJson) ==
           BaselineStatus::kIncompatibleArtifact);

    std::string bad_algorithm_json = artifact_json;
    const std::string algorithm_needle = "\"algorithm_version\":\"b1-bootstrap-v1\"";
    const std::size_t algorithm_pos = bad_algorithm_json.find(algorithm_needle);
    assert(algorithm_pos != std::string::npos);
    bad_algorithm_json.replace(algorithm_pos,
                               algorithm_needle.size(),
                               "\"algorithm_version\":\"unknown\"");
    assert(value_task->LoadBootstrapArtifact(
               bad_algorithm_json, BaselineSerializationFormat::kJson) ==
           BaselineStatus::kIncompatibleArtifact);

    std::string bad_top_task_json = artifact_json;
    const std::string top_task_needle =
        "\"task_identity\":{\"task_id\":\"baseline_task_bps\"";
    const std::size_t top_task_pos = bad_top_task_json.find(top_task_needle);
    assert(top_task_pos != std::string::npos);
    bad_top_task_json.replace(
        top_task_pos,
        top_task_needle.size(),
        "\"task_identity\":{\"task_id\":\"baseline_task_other\"");
    assert(value_task->LoadBootstrapArtifact(
               bad_top_task_json, BaselineSerializationFormat::kJson) ==
           BaselineStatus::kIncompatibleArtifact);

    std::printf("[PASS] B1 task rejects incompatible bootstrap artifact\n");
}

void TestRuntimeConfigHarmonicOrders() {
    std::printf("[TEST] B1 runtime config harmonic orders...\n");

    const std::string config_path = "/tmp/flowsql_baseline_harmonic_order_test.yaml";
    {
        std::ofstream file(config_path);
        file << R"(
baseline:
  parser:
    tz_default: "UTC"
  shared_profile_config:
    daily_harmonic_order: 8
    weekly_harmonic_order: 5
    dme_max: 7
    m_month_enable: 4
    month_cov_min: 0.8
    lambda_season: 1.0
    lambda_dom: 4.0
    lambda_dme: 2.0
    lambda_lwd: 1.0
    lambda_event: 2.0
  value_sampled_profiles:
    cont_core:
      n_train_min: 50
      transform_name_override: "log1p"
  ratio_profiles:
    global:
      eps_logit: 1.0e-4
      m_floor: 1.0e-4
      v_floor: 0.25
    rate_core:
      d_min_train: 50
      s_prior: 2.0
      phi_over: 1.5
  solver_constants:
    solver_name: "weighted_huber_ridge_irls"
    c_huber: 1.5
    s_min_fit: 1.0e-3
    max_iter_fit: 15
    tol_obj_rel: 1.0e-4
    tol_beta_inf: 1.0e-5
    cond_max: 1.0e8
)";
        assert(file.good());
    }

    auto env = LoadBaselineService("config_file=" + config_path + ";strict=true");
    auto [value_status, value_task] = env.service->CreateValueTask(
        ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(value_status == BaselineStatus::kOk);
    assert(value_task != nullptr);
    const BootstrapTrainResult train = value_task->Bootstrap(BuildValueHistory());
    assert(train.status == BaselineStatus::kOk);

    auto [seed_status, seed_json] =
        value_task->ExportBootstrapSeed(BaselineSerializationFormat::kJson);
    assert(seed_status == BaselineStatus::kOk);

    rapidjson::Document doc;
    doc.Parse(seed_json.c_str());
    assert(!doc.HasParseError());
    assert(doc.HasMember("series_seeds"));
    assert(doc["series_seeds"].IsArray());
    assert(doc["series_seeds"].Size() == 1);
    const auto& seed_item = doc["series_seeds"][0];
    assert(seed_item.HasMember("theta_init"));
    assert(seed_item["theta_init"].HasMember("daily_harmonic"));
    assert(seed_item["theta_init"].HasMember("weekly_harmonic"));
    assert(seed_item["theta_init"]["daily_harmonic"].IsArray());
    assert(seed_item["theta_init"]["weekly_harmonic"].IsArray());
    assert(seed_item["theta_init"]["daily_harmonic"].Size() == 8);
    assert(seed_item["theta_init"]["weekly_harmonic"].Size() == 5);

    std::printf("[PASS] B1 runtime config harmonic orders\n");
}

void TestRuntimeConfigDefaultDailyHarmonicOrder() {
    std::printf("[TEST] B1 runtime config default daily harmonic order...\n");

    auto env = LoadBaselineService();
    auto [value_status, value_task] = env.service->CreateValueTask(
        ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(value_status == BaselineStatus::kOk);
    assert(value_task != nullptr);
    const BootstrapTrainResult train = value_task->Bootstrap(BuildValueHistory());
    assert(train.status == BaselineStatus::kOk);

    auto [seed_status, seed_json] =
        value_task->ExportBootstrapSeed(BaselineSerializationFormat::kJson);
    assert(seed_status == BaselineStatus::kOk);

    rapidjson::Document doc;
    doc.Parse(seed_json.c_str());
    assert(!doc.HasParseError());
    assert(doc.HasMember("series_seeds"));
    assert(doc["series_seeds"].IsArray());
    assert(doc["series_seeds"].Size() == 1);
    const auto& seed_item = doc["series_seeds"][0];
    assert(seed_item.HasMember("theta_init"));
    assert(seed_item["theta_init"].HasMember("daily_harmonic"));
    assert(seed_item["theta_init"]["daily_harmonic"].IsArray());
    assert(seed_item["theta_init"]["daily_harmonic"].Size() == 6);

    std::printf("[PASS] B1 runtime config default daily harmonic order\n");
}

void TestB2ValueRollingEmptyStart() {
    std::printf("[TEST] B2 value rolling empty start...\n");

    auto env = LoadBaselineService();
    auto [status, task] =
        env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(task != nullptr);

    ValueRollingObservation first;
    first.series_key = "link-a";
    first.bucket_id = 100;
    first.value = 99.0;
    RollingBaselineResult first_result = task->SubmitObservation(first, RollingSubmitOptions{});
    assert(first_result.status == BaselineStatus::kOk);
    assert(first_result.series_key == "link-a");
    assert(first_result.bucket_id == 100);
    assert(!first_result.can_score);
    assert(first_result.can_update);
    assert(first_result.state_status == "cold_learning");
    assert(first_result.maturity_status == "cold_learning");
    assert(first_result.score_trust_status == "score_untrusted");
    assert(first_result.calibration_status == "uncalibrated");
    assert(first_result.learning_confidence > 0.0);
    assert(first_result.score_confidence == 0.0);
    assert(first_result.effective_confidence == 0.0);
    assert(!first_result.can_alert);
    assert(first_result.enabled_components.empty());
    assert(!first_result.component_readiness.empty());
    assert(first_result.band_width > 0.0);
    assert(first_result.baseline_lower >= 0.0);

    ValueRollingObservation second = first;
    second.bucket_id = 101;
    second.value = 110.0;
    RollingBaselineResult second_result =
        task->SubmitObservation(second, RollingSubmitOptions{});
    assert(second_result.status == BaselineStatus::kOk);
    assert(second_result.can_score);
    assert(second_result.can_update);
    assert(!second_result.maturity_status.empty());
    assert(!second_result.score_trust_status.empty());
    assert(!second_result.calibration_status.empty());
    assert(!second_result.can_alert);
    assert(second_result.update_weight > 0.0);
    assert(second_result.band_width > 0.0);

    std::printf("[PASS] B2 value rolling empty start\n");
}

void TestB2SampledValueLowSupportStart() {
    std::printf("[TEST] B2 sampled value low support start...\n");

    auto env = LoadBaselineService();
    auto [status, task] = env.service->CreateValueTask(SampledValueTaskConfig(),
                                                       BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(task != nullptr);

    ValueRollingObservation low;
    low.series_key = "sampled-a";
    low.bucket_id = 100;
    low.value = 10.0;
    low.sample_count = 2;
    RollingBaselineResult low_result = task->SubmitObservation(low, RollingSubmitOptions{});
    assert(low_result.status == BaselineStatus::kInsufficientData);
    assert(low_result.skipped_low_sample_count);

    ValueRollingObservation ok = low;
    ok.bucket_id = 101;
    ok.sample_count = 20;
    RollingBaselineResult ok_result = task->SubmitObservation(ok, RollingSubmitOptions{});
    assert(ok_result.status == BaselineStatus::kOk);
    assert(ok_result.can_update);
    assert(ok_result.state_status == "cold_learning");

    std::printf("[PASS] B2 sampled value low support start\n");
}

void TestB2RatioRollingInputAndEmptyStart() {
    std::printf("[TEST] B2 ratio rolling input and empty start...\n");

    auto env = LoadBaselineService();
    auto [status, task] =
        env.service->CreateRatioTask(RatioTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(task != nullptr);

    RatioRollingObservation invalid;
    invalid.series_key = "svc-a";
    invalid.bucket_id = 100;
    invalid.numerator = 11.0;
    invalid.denominator = 10.0;
    RollingBaselineResult invalid_result =
        task->SubmitObservation(invalid, RollingSubmitOptions{});
    assert(invalid_result.status == BaselineStatus::kInvalidArgument);

    RatioRollingObservation first;
    first.series_key = "svc-a";
    first.bucket_id = 101;
    first.numerator = 95.0;
    first.denominator = 100.0;
    RollingBaselineResult first_result = task->SubmitObservation(first, RollingSubmitOptions{});
    assert(first_result.status == BaselineStatus::kOk);
    assert(!first_result.can_score);
    assert(first_result.can_update);
    assert(first_result.baseline_lower >= 0.0);
    assert(first_result.baseline_upper <= 1.0);
    assert(first_result.band_width > 0.0);

    std::printf("[PASS] B2 ratio rolling input and empty start\n");
}

void TestB2RollingSnapshotAndBootstrapWarmup() {
    std::printf("[TEST] B2 rolling snapshot and bootstrap warm-up...\n");

    auto env = LoadBaselineService();
    auto [status, task] =
        env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(task != nullptr);

    const BootstrapTrainResult train = task->Bootstrap(BuildValueHistory());
    assert(train.status == BaselineStatus::kOk);

    auto [task_snapshot_status, task_snapshot_json] =
        task->QueryTaskSnapshot(BaselineSerializationFormat::kJson);
    assert(task_snapshot_status == BaselineStatus::kOk);
    rapidjson::Document task_doc;
    task_doc.Parse(task_snapshot_json.c_str());
    assert(!task_doc.HasParseError());
    assert(std::string(task_doc["document_kind"].GetString()) == "rolling_task_snapshot");
    assert(task_doc["rolling_series_count"].GetUint64() == 1);
    assert(task_doc.HasMember("state_status_counts"));
    assert(task_doc.HasMember("maturity_status_counts"));
    assert(task_doc.HasMember("score_trust_status_counts"));
    assert(task_doc.HasMember("calibration_status_counts"));
    assert(task_doc.HasMember("rolling_state_memory_estimate_bytes"));

    auto [series_snapshot_status, series_snapshot_json] =
        task->QuerySeriesSnapshot("svc-a", BaselineSerializationFormat::kJson);
    assert(series_snapshot_status == BaselineStatus::kOk);
    rapidjson::Document series_doc;
    series_doc.Parse(series_snapshot_json.c_str());
    assert(!series_doc.HasParseError());
    assert(std::string(series_doc["document_kind"].GetString()) == "rolling_series_snapshot");
    assert(series_doc["series_identity"]["series_key"] == "svc-a");
    assert(series_doc.HasMember("band"));
    assert(series_doc.HasMember("control"));
    assert(series_doc.HasMember("maturity"));
    assert(series_doc["maturity"].HasMember("status"));
    assert(series_doc["maturity"].HasMember("enabled_components"));
    assert(series_doc["maturity"].HasMember("component_readiness"));
    assert(series_doc["maturity"].HasMember("coverage"));
    assert(series_doc.HasMember("score_trust"));
    assert(series_doc["score_trust"].HasMember("status"));
    assert(series_doc["score_trust"].HasMember("can_alert"));
    assert(series_doc.HasMember("calibration"));
    assert(series_doc["calibration"].HasMember("band_multiplier"));
    assert(series_doc["calibration"].HasMember("calibration_update_count"));
    assert(series_doc.HasMember("monthpos"));
    assert(series_doc["monthpos"].HasMember("status"));

    ValueRollingObservation stream;
    stream.series_key = "svc-a";
    stream.bucket_id = 200;
    stream.value = 105.0;
    RollingBaselineResult result = task->SubmitObservation(stream, RollingSubmitOptions{});
    assert(result.status == BaselineStatus::kOk);
    assert(result.can_score);
    assert(result.state_status == "warming");
    assert(!result.maturity_status.empty());
    assert(!result.score_trust_status.empty());
    assert(!result.calibration_status.empty());
    assert(result.score_trust_status != "score_ready");
    assert(!result.can_alert);

    std::printf("[PASS] B2 rolling snapshot and bootstrap warm-up\n");
}

void TestB2RollingPredict() {
    std::printf("[TEST] B2 rolling predict...\n");

    auto env = LoadBaselineService();
    auto [status, task] =
        env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(task != nullptr);

    RollingPrediction missing = task->PredictRolling("link-predict", 101);
    assert(missing.status == BaselineStatus::kNotTrained);

    ValueRollingObservation first;
    first.series_key = "link-predict";
    first.bucket_id = 100;
    first.value = 100.0;
    assert(task->SubmitObservation(first, RollingSubmitOptions{}).status ==
           BaselineStatus::kOk);

    RollingPrediction past = task->PredictRolling("link-predict", 99);
    assert(past.status == BaselineStatus::kInvalidArgument);

    RollingPrediction prediction = task->PredictRolling("link-predict", 101);
    assert(prediction.status == BaselineStatus::kOk);
    assert(prediction.baseline_mu > 0.0);
    assert(prediction.baseline_lower <= prediction.baseline_mu);
    assert(prediction.baseline_upper >= prediction.baseline_mu);
    assert(prediction.baseline_upper > prediction.baseline_lower);
    assert(prediction.band_z == 3.0);

    ValueRollingObservation second = first;
    second.bucket_id = 101;
    second.value = 102.0;
    RollingBaselineResult update = task->SubmitObservation(second, RollingSubmitOptions{});
    assert(update.status == BaselineStatus::kOk);
    assert(update.can_score);

    auto [ratio_status, ratio_task] =
        env.service->CreateRatioTask(RatioTaskConfig(), BaselineSerializationFormat::kJson);
    assert(ratio_status == BaselineStatus::kOk);
    assert(ratio_task != nullptr);

    RatioRollingObservation ratio_first;
    ratio_first.series_key = "ratio-predict";
    ratio_first.bucket_id = 100;
    ratio_first.numerator = 80.0;
    ratio_first.denominator = 100.0;
    assert(ratio_task->SubmitObservation(ratio_first, RollingSubmitOptions{}).status ==
           BaselineStatus::kOk);

    RollingPrediction ratio_prediction = ratio_task->PredictRolling("ratio-predict", 101);
    assert(ratio_prediction.status == BaselineStatus::kOk);
    assert(ratio_prediction.baseline_lower >= 0.0);
    assert(ratio_prediction.baseline_upper <= 1.0);
    assert(ratio_prediction.baseline_lower <= ratio_prediction.baseline_mu);
    assert(ratio_prediction.baseline_upper >= ratio_prediction.baseline_mu);
    assert(ratio_prediction.band_z == 3.0);

    std::printf("[PASS] B2 rolling predict\n");
}

void TestB2RollingFailureSemantics() {
    std::printf("[TEST] B2 rolling failure semantics...\n");

    {
        auto env = LoadBaselineService();
        auto [status, task] =
            env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        ValueRollingObservation obs;
        obs.series_key = "link-disabled";
        obs.bucket_id = 10;
        obs.value = 1.0;
        RollingSubmitOptions options;
        options.allow_auto_init_from_bootstrap = false;
        options.allow_auto_init_from_empty = false;
        RollingBaselineResult result = task->SubmitObservation(obs, options);
        assert(result.status == BaselineStatus::kNotTrained);
    }

    {
        auto env = LoadBaselineService();
        auto [status, task] =
            env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        ValueRollingObservation obs;
        obs.series_key = "link-dup";
        obs.bucket_id = 10;
        obs.value = 10.0;
        assert(task->SubmitObservation(obs, RollingSubmitOptions{}).status ==
               BaselineStatus::kOk);
        assert(task->SubmitObservation(obs, RollingSubmitOptions{}).status ==
               BaselineStatus::kInvalidArgument);
    }

    {
        auto env = LoadBaselineService();
        auto [status, task] = env.service->CreateValueTask(SampledValueTaskConfig(),
                                                           BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        ValueRollingObservation obs;
        obs.series_key = "sampled-existing";
        obs.bucket_id = 10;
        obs.value = 10.0;
        obs.sample_count = 20;
        assert(task->SubmitObservation(obs, RollingSubmitOptions{}).status ==
               BaselineStatus::kOk);
        obs.bucket_id = 11;
        obs.sample_count = 2;
        RollingBaselineResult low = task->SubmitObservation(obs, RollingSubmitOptions{});
        assert(low.status == BaselineStatus::kOk);
        assert(low.skipped_low_sample_count);
        assert(!low.can_update);
    }

    {
        auto env = LoadBaselineService();
        auto [status, task] =
            env.service->CreateRatioTask(RatioTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        RatioRollingObservation obs;
        obs.series_key = "ratio-low-den";
        obs.bucket_id = 10;
        obs.numerator = 1.0;
        obs.denominator = 5.0;
        RollingBaselineResult result = task->SubmitObservation(obs, RollingSubmitOptions{});
        assert(result.status == BaselineStatus::kInsufficientData);
        assert(result.skipped_low_denominator);
    }

    std::printf("[PASS] B2 rolling failure semantics\n");
}

void TestB3RollingResultUsesPreUpdateTrustSnapshot() {
    std::printf("[TEST] B3 rolling result uses pre-update trust snapshot...\n");

    const std::string config_path = "/tmp/flowsql_baseline_b3_pre_update_test.yaml";
    {
        std::ofstream file(config_path);
        file << R"(
baseline:
  rolling_config:
    min_warming_updates: 1
    min_ready_hint_updates: 3
    level_ready_min_updates: 3
    score_warming_min_updates: 4
    score_ready_min_updates: 5
    score_recovery_min_updates: 2
    calibration_warmup_min_updates: 1
)";
        assert(file.good());
    }

    auto env = LoadBaselineService("config_file=" + config_path + ";strict=false");
    auto [status, task] =
        env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(task != nullptr);

    ValueRollingObservation obs;
    obs.series_key = "pre-update-link";
    obs.bucket_id = 100;
    obs.value = 100.0;
    assert(task->SubmitObservation(obs, RollingSubmitOptions{}).status == BaselineStatus::kOk);
    obs.bucket_id = 101;
    assert(task->SubmitObservation(obs, RollingSubmitOptions{}).status == BaselineStatus::kOk);

    obs.bucket_id = 102;
    RollingBaselineResult boundary = task->SubmitObservation(obs, RollingSubmitOptions{});
    assert(boundary.status == BaselineStatus::kOk);
    assert(boundary.maturity_status == "cold_learning");
    assert(boundary.score_trust_status == "score_untrusted");
    assert(!boundary.can_alert);

    auto [snapshot_status, snapshot_json] =
        task->QuerySeriesSnapshot("pre-update-link", BaselineSerializationFormat::kJson);
    assert(snapshot_status == BaselineStatus::kOk);
    rapidjson::Document snapshot;
    snapshot.Parse(snapshot_json.c_str());
    assert(!snapshot.HasParseError());
    assert(std::string(snapshot["maturity_status"].GetString()) != "cold_learning");

    std::printf("[PASS] B3 rolling result uses pre-update trust snapshot\n");
}

}  // namespace

int main() {
    TestEventCalendarSchemaRejectsTaskScopedFields();
    TestCreateTaskUsesConfigIdentity();
    TestInvalidTaskConfigRejected();
    TestTaskBootstrapPredictAndExport();
    TestValueTaskKeepsBootstrapPerSeriesAndExportsAll();
    TestBootstrapUsesConfiguredEventCalendar();
    TestTaskRejectsIncompatibleBootstrapArtifact();
    TestRuntimeConfigDefaultDailyHarmonicOrder();
    TestRuntimeConfigHarmonicOrders();
    TestB2ValueRollingEmptyStart();
    TestB2SampledValueLowSupportStart();
    TestB2RatioRollingInputAndEmptyStart();
    TestB2RollingSnapshotAndBootstrapWarmup();
    TestB2RollingPredict();
    TestB2RollingFailureSemantics();
    TestB3RollingResultUsesPreUpdateTrustSnapshot();
    TestB4RelationSubmitObservation();
    TestB4RelationRollingConfigSwitches();
    return 0;
}
