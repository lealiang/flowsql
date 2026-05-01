/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <framework/interfaces/ibaseline_service.h>
#include <plugins/baseline/bootstrap/bootstrap_engine.h>
#include <plugins/baseline/task/baseline_task_base.h>
#include <plugins/baseline/task/relation_task.h>
#include <plugins/baseline/task/ratio_task.h>
#include <plugins/baseline/task/value_task.h>
#include <rapidjson/document.h>

using namespace flowsql;
using namespace flowsql::baseline;

namespace {

void AssertNear(double actual, double expected) {
    const double diff = actual > expected ? actual - expected : expected - actual;
    assert(diff < 1.0e-9);
}

template <typename T, typename = void>
struct HasRequestRebuild : std::false_type {};

template <typename T>
struct HasRequestRebuild<T, std::void_t<decltype(&T::RequestRebuild)>> : std::true_type {};

template <typename T, typename = void>
struct HasValueSubmitObservation : std::false_type {};

template <typename T>
struct HasValueSubmitObservation<T, std::void_t<decltype(&T::SubmitObservation)>>
    : std::true_type {};

template <typename T, typename = void>
struct HasValuePredictRolling : std::false_type {};

template <typename T>
struct HasValuePredictRolling<T, std::void_t<decltype(&T::PredictRolling)>>
    : std::true_type {};

template <typename T, typename = void>
struct HasValueInitRollingFromEmpty : std::false_type {};

template <typename T>
struct HasValueInitRollingFromEmpty<T,
                                    std::void_t<decltype(&T::InitRollingFromEmpty)>>
    : std::true_type {};

template <typename T, typename = void>
struct HasValueInitRollingFromBootstrap : std::false_type {};

template <typename T>
struct HasValueInitRollingFromBootstrap<
    T,
    std::void_t<decltype(&T::InitRollingFromBootstrap)>> : std::true_type {};

template <typename T, typename = void>
struct HasRatioSubmitObservation : std::false_type {};

template <typename T>
struct HasRatioSubmitObservation<T, std::void_t<decltype(&T::SubmitObservation)>>
    : std::true_type {};

template <typename T, typename = void>
struct HasRatioPredictRolling : std::false_type {};

template <typename T>
struct HasRatioPredictRolling<T, std::void_t<decltype(&T::PredictRolling)>>
    : std::true_type {};

template <typename T, typename = void>
struct HasRatioInitRollingFromEmpty : std::false_type {};

template <typename T>
struct HasRatioInitRollingFromEmpty<T,
                                    std::void_t<decltype(&T::InitRollingFromEmpty)>>
    : std::true_type {};

template <typename T, typename = void>
struct HasRatioInitRollingFromBootstrap : std::false_type {};

template <typename T>
struct HasRatioInitRollingFromBootstrap<
    T,
    std::void_t<decltype(&T::InitRollingFromBootstrap)>> : std::true_type {};

template <typename T, typename = void>
struct HasRelationSubmitBlock : std::false_type {};

template <typename T>
struct HasRelationSubmitBlock<T, std::void_t<decltype(&T::SubmitBlock)>>
    : std::true_type {};

template <typename T, typename = void>
struct HasSetHistoryReader : std::false_type {};

template <typename T>
struct HasSetHistoryReader<T, std::void_t<decltype(&T::SetHistoryReader)>> : std::true_type {};

template <typename T, typename = void>
struct HasQueryBootstrapSeed : std::false_type {};

template <typename T>
struct HasQueryBootstrapSeed<T, std::void_t<decltype(&T::QueryBootstrapSeed)>>
    : std::true_type {};

void TestTaskHeaderContracts() {
    std::printf("[TEST] Task header contracts...\n");

    static_assert(std::is_base_of_v<BaselineTaskBase, BaselineValueTask>);
    static_assert(std::is_base_of_v<BaselineTaskBase, BaselineRatioTask>);
    static_assert(std::is_base_of_v<BaselineTaskBase, BaselineRelationTask>);

    using ExpectedCreateValueTask =
        std::pair<BaselineStatus, std::shared_ptr<IBaselineValueTask>> (
            IBaselineService::*)(std::string_view, BaselineSerializationFormat);
    using ExpectedCreateRatioTask =
        std::pair<BaselineStatus, std::shared_ptr<IBaselineRatioTask>> (
            IBaselineService::*)(std::string_view, BaselineSerializationFormat);
    using ExpectedCreateRelationTask =
        std::pair<BaselineStatus, std::shared_ptr<IBaselineRelationTask>> (
            IBaselineService::*)(std::string_view, BaselineSerializationFormat);
    using ExpectedValuePredictRolling =
        RollingPrediction (IBaselineValueTask::*)(std::string_view, int64_t) const;
    using ExpectedRatioPredictRolling =
        RollingPrediction (IBaselineRatioTask::*)(std::string_view, int64_t) const;

    static_assert(std::is_same_v<decltype(&IBaselineService::CreateValueTask),
                                 ExpectedCreateValueTask>);
    static_assert(std::is_same_v<decltype(&IBaselineService::CreateRatioTask),
                                 ExpectedCreateRatioTask>);
    static_assert(std::is_same_v<decltype(&IBaselineService::CreateRelationTask),
                                 ExpectedCreateRelationTask>);
    static_assert(std::is_same_v<decltype(&IBaselineValueTask::PredictRolling),
                                 ExpectedValuePredictRolling>);
    static_assert(std::is_same_v<decltype(&IBaselineRatioTask::PredictRolling),
                                 ExpectedRatioPredictRolling>);

    static_assert(!HasRequestRebuild<IBaselineTask>::value);
    static_assert(!HasSetHistoryReader<IBaselineValueTask>::value);
    static_assert(!HasSetHistoryReader<IBaselineRatioTask>::value);
    static_assert(!HasSetHistoryReader<IBaselineRelationTask>::value);
    static_assert(!HasValueInitRollingFromEmpty<IBaselineValueTask>::value);
    static_assert(!HasValueInitRollingFromBootstrap<IBaselineValueTask>::value);
    static_assert(HasValueSubmitObservation<IBaselineValueTask>::value);
    static_assert(HasValuePredictRolling<IBaselineValueTask>::value);
    static_assert(!HasRatioInitRollingFromEmpty<IBaselineRatioTask>::value);
    static_assert(!HasRatioInitRollingFromBootstrap<IBaselineRatioTask>::value);
    static_assert(HasRatioSubmitObservation<IBaselineRatioTask>::value);
    static_assert(HasRatioPredictRolling<IBaselineRatioTask>::value);
    static_assert(!HasValueInitRollingFromEmpty<IBaselineRelationTask>::value);
    static_assert(!HasValueInitRollingFromBootstrap<IBaselineRelationTask>::value);
    static_assert(!HasValueSubmitObservation<IBaselineRelationTask>::value);
    static_assert(!HasValuePredictRolling<IBaselineRelationTask>::value);
    static_assert(!HasRatioInitRollingFromEmpty<IBaselineRelationTask>::value);
    static_assert(!HasRatioInitRollingFromBootstrap<IBaselineRelationTask>::value);
    static_assert(!HasRatioSubmitObservation<IBaselineRelationTask>::value);
    static_assert(!HasRatioPredictRolling<IBaselineRelationTask>::value);
    static_assert(!HasRelationSubmitBlock<IBaselineRelationTask>::value);
    static_assert(!HasQueryBootstrapSeed<IBaselineValueTask>::value);
    static_assert(!HasQueryBootstrapSeed<IBaselineRatioTask>::value);
    static_assert(!HasQueryBootstrapSeed<IBaselineRelationTask>::value);
    static_assert(!HasQueryBootstrapSeed<BaselineValueTask>::value);
    static_assert(!HasQueryBootstrapSeed<BaselineRatioTask>::value);
    static_assert(!HasQueryBootstrapSeed<BaselineRelationTask>::value);

    RollingSubmitOptions rolling_submit_options;
    ValueRollingObservation value_obs;
    RatioRollingObservation ratio_obs;
    RollingBaselineResult rolling_result;
    RollingPrediction rolling_prediction;
    assert(rolling_submit_options.allow_auto_init_from_bootstrap);
    assert(rolling_submit_options.allow_auto_init_from_empty);
    assert(value_obs.sample_count == 0);
    assert(ratio_obs.denominator == 0.0);
    assert(rolling_result.status == BaselineStatus::kOk);
    assert(rolling_prediction.status == BaselineStatus::kOk);
    AssertNear(rolling_prediction.baseline_mu, 0.0);
    AssertNear(rolling_prediction.baseline_lower, 0.0);
    AssertNear(rolling_prediction.baseline_upper, 0.0);
    AssertNear(rolling_prediction.band_z, 0.0);

    std::printf("[PASS] Task header contracts\n");
}

void TestBootstrapEngineTrainsValueAndRatio() {
    std::printf("[TEST] Bootstrap engine trains value and ratio...\n");

    BootstrapEngine engine;
    BaselineTaskSpec value_spec;
    value_spec.task_id = "bootstrap-value-task";
    value_spec.feature_id = "bps";
    value_spec.task_kind = "value";
    value_spec.feature_type = "value_basic";
    value_spec.profile = "default";
    value_spec.clock_spec.bucket_seconds = 60;
    value_spec.clock_spec.timezone = "UTC";
    value_spec.calendar_ref.calendar_id = "test-calendar";
    value_spec.calendar_ref.calendar_version = "v1";

    ValueBootstrapInput input;
    input.series_key = "svc-a";
    for (int64_t bucket = 0; bucket < 200; ++bucket) {
        input.observations.push_back(
            ValueBootstrapPoint{bucket, 100.0 + static_cast<double>(bucket % 10), 1});
    }

    BootstrapArtifact value_artifact;
    const BootstrapTrainResult value_result =
        engine.TrainValue(value_spec, input, &value_artifact);
    assert(value_result.status == BaselineStatus::kOk);
    assert(value_result.accepted_count == input.observations.size());
    assert(value_artifact.artifact_kind == BootstrapArtifactKind::kValue);
    assert(value_artifact.train_status == BaselineStatus::kOk);
    assert(value_artifact.value_model != nullptr);

    BootstrapPredictionOptions prediction_options;
    prediction_options.include_model_space_debug = true;
    const BootstrapPrediction value_prediction =
        engine.PredictValue(value_artifact, 220, prediction_options);
    assert(value_prediction.status == BaselineStatus::kOk);
    assert(value_prediction.baseline_mu > 0.0);
    assert(value_prediction.baseline_lower <= value_prediction.baseline_mu);
    assert(value_prediction.baseline_upper >= value_prediction.baseline_mu);
    assert(value_prediction.band_width > 0.0);
    assert(value_prediction.has_model_space);

    auto [artifact_status, artifact_json] =
        engine.ExportArtifact(value_artifact, BaselineSerializationFormat::kJson);
    assert(artifact_status == BaselineStatus::kOk);
    assert(artifact_json.find("\"document_kind\":\"bootstrap_artifact\"") != std::string::npos);

    BootstrapArtifact loaded_value_artifact;
    assert(engine.LoadArtifact(
               artifact_json, BaselineSerializationFormat::kJson, &loaded_value_artifact) ==
           BaselineStatus::kOk);
    const BootstrapPrediction loaded_value_prediction =
        engine.PredictValue(loaded_value_artifact, 220, prediction_options);
    assert(loaded_value_prediction.status == BaselineStatus::kOk);
    assert(loaded_value_prediction.baseline_mu > 0.0);

    BootstrapSeed value_seed;
    assert(engine.ExportSeed(value_artifact, &value_seed) == BaselineStatus::kOk);
    auto [seed_status, seed_json] =
        engine.ExportSeed(value_seed, BaselineSerializationFormat::kJson);
    assert(seed_status == BaselineStatus::kOk);
    assert(seed_json.find("\"document_kind\":\"bootstrap_seed\"") != std::string::npos);
    assert(seed_json.find("\"algorithm_version\":\"b1-bootstrap-v1\"") != std::string::npos);
    assert(seed_json.find("\"feature_type\":\"value_basic\"") != std::string::npos);
    assert(seed_json.find("\"calendar_ref\"") != std::string::npos);
    assert(seed_json.find("\"clock_spec\"") != std::string::npos);
    assert(seed_json.find("\"bucket_seconds\":60") != std::string::npos);
    assert(seed_json.find("\"timezone\":\"UTC\"") != std::string::npos);
    assert(seed_json.find("\"seeded_components\"") != std::string::npos);
    assert(seed_json.find("\"enabled_components\"") != std::string::npos);
    assert(seed_json.find("\"level\"") != std::string::npos);
    assert(seed_json.find("\"trend\"") != std::string::npos);
    assert(seed_json.find("\"daily\"") != std::string::npos);
    assert(seed_json.find("\"weekly\"") != std::string::npos);
    assert(seed_json.find("\"core\"") == std::string::npos);
    assert(seed_json.find("\"theta_init\"") != std::string::npos);
    assert(seed_json.find("\"level\"") != std::string::npos);
    assert(seed_json.find("\"trend\"") != std::string::npos);
    assert(seed_json.find("\"daily_harmonic\"") != std::string::npos);
    assert(seed_json.find("\"weekly_harmonic\"") != std::string::npos);
    assert(seed_json.find("\"sigma_init\"") != std::string::npos);
    assert(seed_json.find("\"uncertainty_init\"") != std::string::npos);
    assert(seed_json.find("\"component_uncertainty\"") != std::string::npos);
    assert(seed_json.find("\"level_scale\"") != std::string::npos);
    assert(seed_json.find("\"trend_scale\"") != std::string::npos);
    assert(seed_json.find("\"daily_scale\"") != std::string::npos);
    assert(seed_json.find("\"weekly_scale\"") != std::string::npos);
    assert(seed_json.find("\"maturity_init\"") != std::string::npos);
    assert(seed_json.find("\"model\"") == std::string::npos);

    BaselineTaskSpec sampled_value_spec = value_spec;
    sampled_value_spec.task_id = "bootstrap-sampled-value-task";
    sampled_value_spec.feature_type = "value_sampled";
    sampled_value_spec.profile = "cont_core";
    ValueBootstrapInput sampled_input = input;
    for (auto& point : sampled_input.observations) {
        point.sample_count = 50;
    }
    BootstrapArtifact sampled_value_artifact;
    const BootstrapTrainResult sampled_value_result =
        engine.TrainValue(sampled_value_spec, sampled_input, &sampled_value_artifact);
    assert(sampled_value_result.status == BaselineStatus::kOk);
    assert(sampled_value_artifact.value_model != nullptr);

    BaselineTaskSpec invalid_basic_spec = value_spec;
    invalid_basic_spec.task_id = "bootstrap-invalid-basic-task";
    invalid_basic_spec.feature_type = "value_basic";
    invalid_basic_spec.profile = "cont_core";
    BootstrapArtifact invalid_basic_artifact;
    const BootstrapTrainResult invalid_basic_result =
        engine.TrainValue(invalid_basic_spec, sampled_input, &invalid_basic_artifact);
    assert(invalid_basic_result.status == BaselineStatus::kInvalidArgument);

    BaselineTaskSpec invalid_sampled_spec = value_spec;
    invalid_sampled_spec.task_id = "bootstrap-invalid-sampled-task";
    invalid_sampled_spec.feature_type = "value_sampled";
    invalid_sampled_spec.profile = "default";
    BootstrapArtifact invalid_sampled_artifact;
    const BootstrapTrainResult invalid_sampled_result =
        engine.TrainValue(invalid_sampled_spec, sampled_input, &invalid_sampled_artifact);
    assert(invalid_sampled_result.status == BaselineStatus::kInvalidArgument);

    BaselineTaskSpec ratio_spec;
    ratio_spec.task_id = "bootstrap-ratio-task";
    ratio_spec.feature_id = "success_rate";
    ratio_spec.task_kind = "ratio";
    ratio_spec.feature_type = "ratio";
    ratio_spec.profile = "rate_core";
    ratio_spec.clock_spec.bucket_seconds = 60;
    ratio_spec.clock_spec.timezone = "UTC";
    ratio_spec.calendar_ref.calendar_id = "test-calendar";
    ratio_spec.calendar_ref.calendar_version = "v1";

    RatioBootstrapInput ratio_input;
    ratio_input.series_key = "svc-a";
    for (int64_t bucket = 0; bucket < 200; ++bucket) {
        ratio_input.observations.push_back(
            RatioBootstrapPoint{bucket, 95.0 + static_cast<double>(bucket % 3), 100.0});
    }

    BootstrapArtifact ratio_artifact;
    const BootstrapTrainResult ratio_result =
        engine.TrainRatio(ratio_spec, ratio_input, &ratio_artifact);
    assert(ratio_result.status == BaselineStatus::kOk);
    assert(ratio_result.accepted_count == ratio_input.observations.size());
    assert(ratio_artifact.artifact_kind == BootstrapArtifactKind::kRatio);
    assert(ratio_artifact.train_status == BaselineStatus::kOk);
    assert(ratio_artifact.ratio_model != nullptr);

    const BootstrapPrediction ratio_prediction =
        engine.PredictRatio(ratio_artifact, 220, prediction_options);
    assert(ratio_prediction.status == BaselineStatus::kOk);
    assert(ratio_prediction.baseline_mu >= 0.0);
    assert(ratio_prediction.baseline_mu <= 1.0);
    assert(ratio_prediction.baseline_lower >= 0.0);
    assert(ratio_prediction.baseline_upper <= 1.0);
    assert(ratio_prediction.baseline_lower <= ratio_prediction.baseline_mu);
    assert(ratio_prediction.baseline_upper >= ratio_prediction.baseline_mu);
    assert(ratio_prediction.band_width > 0.0);

    BootstrapSeed ratio_seed;
    assert(engine.ExportSeed(ratio_artifact, &ratio_seed) == BaselineStatus::kOk);
    auto [ratio_seed_status, ratio_seed_json] =
        engine.ExportSeed(ratio_seed, BaselineSerializationFormat::kJson);
    assert(ratio_seed_status == BaselineStatus::kOk);
    assert(ratio_seed_json.find("\"feature_type\":\"ratio\"") != std::string::npos);
    assert(ratio_seed_json.find("\"calendar_ref\"") != std::string::npos);
    assert(ratio_seed_json.find("\"clock_spec\"") != std::string::npos);
    assert(ratio_seed_json.find("\"seeded_components\"") != std::string::npos);
    assert(ratio_seed_json.find("\"enabled_components\"") != std::string::npos);
    assert(ratio_seed_json.find("\"theta_init\"") != std::string::npos);
    assert(ratio_seed_json.find("\"sigma_init\"") != std::string::npos);
    assert(ratio_seed_json.find("\"uncertainty_init\"") != std::string::npos);
    assert(ratio_seed_json.find("\"component_uncertainty\"") != std::string::npos);
    assert(ratio_seed_json.find("\"maturity_init\"") != std::string::npos);
    assert(ratio_seed_json.find("\"ratio_prior_init\"") != std::string::npos);
    assert(ratio_seed_json.find("\"m0\"") != std::string::npos);
    assert(ratio_seed_json.find("\"alpha0\"") != std::string::npos);
    assert(ratio_seed_json.find("\"beta0\"") != std::string::npos);
    assert(ratio_seed_json.find("\"model\"") == std::string::npos);

    rapidjson::Document ratio_seed_doc;
    ratio_seed_doc.Parse(ratio_seed_json.c_str());
    assert(!ratio_seed_doc.HasParseError());
    assert(ratio_seed_doc["theta_init"]["model_space"].IsString());
    assert(std::string(ratio_seed_doc["theta_init"]["model_space"].GetString()) == "logit");
    assert(ratio_seed_doc["sigma_init"]["model_space"].IsString());
    assert(std::string(ratio_seed_doc["sigma_init"]["model_space"].GetString()) == "logit");

    std::printf("[PASS] Bootstrap engine trains value and ratio\n");
}

void TestBootstrapEngineNormalizesHistoryBeforeTraining() {
    std::printf("[TEST] Bootstrap engine normalizes duplicate buckets before training...\n");

    BootstrapEngine engine;
    BaselineTaskSpec value_spec;
    value_spec.task_id = "bootstrap-sampled-normalize-task";
    value_spec.feature_id = "sampled_bps";
    value_spec.task_kind = "value";
    value_spec.feature_type = "value_sampled";
    value_spec.profile = "cont_core";
    value_spec.clock_spec.bucket_seconds = 60;
    value_spec.clock_spec.timezone = "UTC";
    value_spec.calendar_ref.calendar_id = "test-calendar";
    value_spec.calendar_ref.calendar_version = "v1";

    ValueBootstrapInput value_input;
    value_input.series_key = "svc-a";
    value_input.observations.push_back(ValueBootstrapPoint{0, 10.0, 25});
    value_input.observations.push_back(ValueBootstrapPoint{0, 30.0, 25});
    value_input.observations.push_back(ValueBootstrapPoint{1, 200.0, 20});
    value_input.observations.push_back(ValueBootstrapPoint{2, 40.0, 50});

    BootstrapArtifact value_artifact;
    const BootstrapTrainResult value_result =
        engine.TrainValue(value_spec, value_input, &value_artifact);
    assert(value_result.status == BaselineStatus::kOk);
    assert(value_result.accepted_count == 2);
    assert(value_result.rejected_count == 1);
    AssertNear(value_result.coverage_ratio, 2.0 / 3.0);
    assert(value_artifact.coverage_report.accepted_count == 2);
    assert(value_artifact.coverage_report.rejected_count == 1);
    AssertNear(value_artifact.coverage_report.coverage_ratio, 2.0 / 3.0);

    BaselineTaskSpec ratio_spec;
    ratio_spec.task_id = "bootstrap-ratio-normalize-task";
    ratio_spec.feature_id = "success_rate";
    ratio_spec.task_kind = "ratio";
    ratio_spec.feature_type = "ratio";
    ratio_spec.profile = "rate_core";
    ratio_spec.clock_spec.bucket_seconds = 60;
    ratio_spec.clock_spec.timezone = "UTC";
    ratio_spec.calendar_ref.calendar_id = "test-calendar";
    ratio_spec.calendar_ref.calendar_version = "v1";

    RatioBootstrapInput ratio_input;
    ratio_input.series_key = "svc-a";
    ratio_input.observations.push_back(RatioBootstrapPoint{0, 10.0, 25.0});
    ratio_input.observations.push_back(RatioBootstrapPoint{0, 15.0, 25.0});
    ratio_input.observations.push_back(RatioBootstrapPoint{1, 1.0, 20.0});
    ratio_input.observations.push_back(RatioBootstrapPoint{2, 40.0, 50.0});

    BootstrapArtifact ratio_artifact;
    const BootstrapTrainResult ratio_result =
        engine.TrainRatio(ratio_spec, ratio_input, &ratio_artifact);
    assert(ratio_result.status == BaselineStatus::kOk);
    assert(ratio_result.accepted_count == 2);
    assert(ratio_result.rejected_count == 1);
    AssertNear(ratio_result.coverage_ratio, 2.0 / 3.0);
    assert(ratio_artifact.coverage_report.accepted_count == 2);
    assert(ratio_artifact.coverage_report.rejected_count == 1);
    AssertNear(ratio_artifact.coverage_report.coverage_ratio, 2.0 / 3.0);

    ValueBootstrapInput min_value_input = value_input;
    min_value_input.options.min_observation_count = 3;
    BootstrapArtifact min_value_artifact;
    const BootstrapTrainResult min_value_result =
        engine.TrainValue(value_spec, min_value_input, &min_value_artifact);
    assert(min_value_result.status == BaselineStatus::kInsufficientData);
    assert(min_value_result.accepted_count == 2);

    RelationTaskCreateSpec relation_spec;
    relation_spec.task_spec.task_id = "relation-bootstrap-normalize-task";
    relation_spec.task_spec.task_kind = "relation";
    relation_spec.task_spec.feature_id = "client_mix";
    relation_spec.task_spec.feature_base = "client_mix";
    relation_spec.task_spec.group_space_id = "client_group";
    relation_spec.task_spec.group_space_version = "v1";
    relation_spec.task_spec.metrics = {"bps"};
    relation_spec.task_spec.support_policy.k_support = 2;
    relation_spec.task_spec.support_policy.min_hist_share = 0.01;
    relation_spec.task_spec.support_policy.min_active_ratio = 0.1;
    relation_spec.task_spec.summary_policy.k_head = 2;
    relation_spec.task_spec.summary_policy.k_stable = 1;
    relation_spec.clock_spec.delta = 60;
    relation_spec.clock_spec.tz = "UTC";

    auto relation_block = [](int64_t bucket,
                             double total,
                             double g1,
                             double g2) {
        RelationBootstrapBlock block;
        block.bucket_id = bucket;
        block.group_idx = {1, 2};
        RelationBootstrapMetric metric;
        metric.metric = "bps";
        metric.total = total;
        metric.values_by_group = {g1, g2};
        block.metrics.push_back(metric);
        return block;
    };

    RelationBootstrapInput relation_input;
    relation_input.series_key = "svc-a";
    relation_input.blocks.push_back(relation_block(0, 100.0, 60.0, 40.0));
    relation_input.blocks.push_back(relation_block(0, 50.0, 20.0, 30.0));
    relation_input.blocks.push_back(relation_block(1, 100.0, 70.0, 30.0));

    BootstrapArtifact relation_artifact;
    const BootstrapTrainResult relation_result =
        engine.TrainRelation(relation_spec, relation_input, &relation_artifact);
    assert(relation_result.status == BaselineStatus::kOk);
    assert(relation_result.accepted_count == 2);
    assert(relation_result.rejected_count == 0);
    AssertNear(relation_result.coverage_ratio, 1.0);
    assert(relation_artifact.coverage_report.accepted_count == 2);
    AssertNear(relation_artifact.coverage_report.coverage_ratio, 1.0);

    RelationBootstrapInput min_relation_input = relation_input;
    min_relation_input.options.min_observation_count = 3;
    BootstrapArtifact min_relation_artifact;
    const BootstrapTrainResult min_relation_result =
        engine.TrainRelation(relation_spec, min_relation_input, &min_relation_artifact);
    assert(min_relation_result.status == BaselineStatus::kInsufficientData);
    assert(min_relation_result.accepted_count == 2);

    std::printf("[PASS] Bootstrap engine normalizes duplicate buckets before training\n");
}

void TestBootstrapEnginePreservesMonthposArtifactAndSeed() {
    std::printf("[TEST] Bootstrap engine preserves monthpos artifact and seed...\n");

    BootstrapEngine engine;
    BaselineTaskSpec value_spec;
    value_spec.task_id = "bootstrap-monthpos-task";
    value_spec.feature_id = "daily_bps";
    value_spec.task_kind = "value";
    value_spec.feature_type = "value_basic";
    value_spec.profile = "default";
    value_spec.clock_spec.bucket_seconds = 86400;
    value_spec.clock_spec.timezone = "UTC";
    value_spec.calendar_ref.calendar_id = "test-calendar";
    value_spec.calendar_ref.calendar_version = "v1";

    ValueBootstrapInput input;
    input.series_key = "svc-monthpos";
    const int64_t start_bucket = 18628;  // 2021-01-01 UTC when bucket_seconds=86400.
    for (int64_t offset = 0; offset < 160; ++offset) {
        const int64_t bucket = start_bucket + offset;
        const int64_t dom = (offset % 31) + 1;
        const double value = 100.0 + (dom == 1 ? 120.0 : 0.0);
        input.observations.push_back(ValueBootstrapPoint{bucket, value, 1});
    }

    BootstrapArtifact artifact;
    const BootstrapTrainResult result = engine.TrainValue(value_spec, input, &artifact);
    assert(result.status == BaselineStatus::kOk);
    assert(artifact.value_model != nullptr);
    assert(artifact.value_model->monthpos_block.enabled);

    BootstrapPredictionOptions options;
    options.include_model_space_debug = true;
    const int64_t predict_bucket = start_bucket + 151;
    const BootstrapPrediction before =
        engine.PredictValue(artifact, predict_bucket, options, &value_spec, nullptr);
    assert(before.status == BaselineStatus::kOk);

    auto [artifact_status, artifact_json] =
        engine.ExportArtifact(artifact, BaselineSerializationFormat::kJson);
    assert(artifact_status == BaselineStatus::kOk);
    assert(artifact_json.find("\"monthpos_block\"") != std::string::npos);

    BootstrapArtifact loaded;
    assert(engine.LoadArtifact(artifact_json, BaselineSerializationFormat::kJson, &loaded) ==
           BaselineStatus::kOk);
    assert(loaded.value_model != nullptr);
    assert(loaded.value_model->monthpos_block.enabled);
    const BootstrapPrediction after =
        engine.PredictValue(loaded, predict_bucket, options, &value_spec, nullptr);
    assert(after.status == BaselineStatus::kOk);
    AssertNear(after.model_space_mu, before.model_space_mu);

    BootstrapSeed seed;
    assert(engine.ExportSeed(artifact, &seed) == BaselineStatus::kOk);
    assert(seed.monthpos_hint.available);
    auto [seed_status, seed_json] =
        engine.ExportSeed(seed, BaselineSerializationFormat::kJson);
    assert(seed_status == BaselineStatus::kOk);
    assert(seed_json.find("\"monthpos_hint\"") != std::string::npos);
    assert(seed_json.find("\"monthpos\"") != std::string::npos);

    std::printf("[PASS] Bootstrap engine preserves monthpos artifact and seed\n");
}

void TestBootstrapEngineTrainsRelationBasis() {
    std::printf("[TEST] Bootstrap engine trains relation basis...\n");

    RelationTaskCreateSpec spec;
    spec.task_spec.task_id = "relation-bootstrap-task";
    spec.task_spec.task_kind = "relation";
    spec.task_spec.feature_id = "client_mix";
    spec.task_spec.feature_base = "client_mix";
    spec.task_spec.group_space_id = "client_group";
    spec.task_spec.group_space_version = "v1";
    spec.task_spec.metrics = {"bps"};
    spec.task_spec.support_policy.k_support = 2;
    spec.task_spec.support_policy.min_hist_share = 0.01;
    spec.task_spec.support_policy.min_active_ratio = 0.1;
    spec.task_spec.summary_policy.k_head = 2;
    spec.task_spec.summary_policy.k_stable = 1;
    spec.task_spec.calendar_ref.calendar_id = "test-calendar";
    spec.task_spec.calendar_ref.calendar_version = "v1";
    spec.clock_spec.delta = 60;
    spec.clock_spec.tz = "UTC";

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

    BootstrapEngine engine;
    BootstrapArtifact artifact;
    const BootstrapTrainResult result = engine.TrainRelation(spec, input, &artifact);
    assert(result.status == BaselineStatus::kOk);
    assert(artifact.artifact_kind == BootstrapArtifactKind::kRelation);
    assert(artifact.relation_basis_by_metric.size() == 1);
    assert(artifact.relation_basis_by_metric[0].support_explicit.size() == 2);
    assert(artifact.relation_basis_by_metric[0].stable_head.size() == 1);
    assert(!artifact.relation_routed_summary_artifacts.empty());

    bool has_value_summary = false;
    bool has_ratio_summary = false;
    for (const auto& routed_artifact : artifact.relation_routed_summary_artifacts) {
        if (routed_artifact.summary_name == "entropy_shannon" &&
            routed_artifact.task_kind == BaselineTaskKind::kValue) {
            has_value_summary = true;
        }
        if (routed_artifact.summary_name == "top1_share" &&
            routed_artifact.task_kind == BaselineTaskKind::kRatio) {
            has_ratio_summary = true;
        }
    }
    assert(has_value_summary);
    assert(has_ratio_summary);

    BootstrapSeed relation_seed;
    assert(engine.ExportSeed(artifact, &relation_seed) == BaselineStatus::kOk);
    assert(!relation_seed.relation_routed_summary_seeds.empty());
    auto [seed_status, seed_json] =
        engine.ExportSeed(relation_seed, BaselineSerializationFormat::kJson);
    assert(seed_status == BaselineStatus::kOk);
    assert(seed_json.find("\"feature_type\":\"relation\"") != std::string::npos);
    assert(seed_json.find("\"feature_type\":\"value_basic\"") != std::string::npos);
    assert(seed_json.find("\"feature_type\":\"ratio\"") != std::string::npos);
    assert(seed_json.find("\"clock_spec\"") != std::string::npos);
    assert(seed_json.find("\"seeded_components\"") != std::string::npos);
    assert(seed_json.find("\"enabled_components\"") != std::string::npos);
    assert(seed_json.find("\"relation_basis\"") != std::string::npos);
    assert(seed_json.find("\"relation_routed_summary_seeds\"") != std::string::npos);
    assert(seed_json.find("\"relation_basis_by_metric\"") != std::string::npos);
    assert(seed_json.find("\"summary\":\"entropy_shannon\"") != std::string::npos);
    assert(seed_json.find("\"summary\":\"top1_share\"") != std::string::npos);
    assert(seed_json.find("\"theta_init\"") != std::string::npos);
    assert(seed_json.find("\"sigma_init\"") != std::string::npos);
    assert(seed_json.find("\"uncertainty_init\"") != std::string::npos);
    assert(seed_json.find("\"component_uncertainty\"") != std::string::npos);
    assert(seed_json.find("\"maturity_init\"") != std::string::npos);
    assert(seed_json.find("\"ratio_prior_init\"") != std::string::npos);
    assert(seed_json.find("\"model\"") == std::string::npos);

    auto [artifact_status, artifact_json] =
        engine.ExportArtifact(artifact, BaselineSerializationFormat::kJson);
    assert(artifact_status == BaselineStatus::kOk);
    BootstrapArtifact loaded_artifact;
    assert(engine.LoadArtifact(
               artifact_json, BaselineSerializationFormat::kJson, &loaded_artifact) ==
           BaselineStatus::kOk);
    assert(loaded_artifact.artifact_kind == BootstrapArtifactKind::kRelation);
    assert(loaded_artifact.relation_basis_by_metric.size() == 1);
    assert(!loaded_artifact.relation_routed_summary_artifacts.empty());
    BootstrapSeed loaded_seed;
    assert(engine.ExportSeed(loaded_artifact, &loaded_seed) == BaselineStatus::kOk);
    assert(!loaded_seed.relation_routed_summary_seeds.empty());

    std::printf("[PASS] Bootstrap engine trains relation basis\n");
}

}  // namespace

int main() {
    TestTaskHeaderContracts();
    TestBootstrapEngineTrainsValueAndRatio();
    TestBootstrapEngineNormalizesHistoryBeforeTraining();
    TestBootstrapEnginePreservesMonthposArtifactAndSeed();
    TestBootstrapEngineTrainsRelationBasis();
    return 0;
}
