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
    return 0;
}
