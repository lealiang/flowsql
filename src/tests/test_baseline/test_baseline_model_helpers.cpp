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
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <framework/interfaces/ibaseline_types.h>
#include <plugins/baseline/config/runtime_config.h>
#include <plugins/baseline/config_parser.h>
#include <plugins/baseline/fusion/relation_pattern_fusion.h>
#include <plugins/baseline/model/calendar_feature_helper.h>
#include <plugins/baseline/model/event_calendar_matcher.h>
#include <plugins/baseline/model/formal_predictor.h>
#include <plugins/baseline/model/profile_config.h>
#include <plugins/baseline/model/readiness_helper.h>
#include <plugins/baseline/model/task_spec.h>
#include <plugins/baseline/task/rebuild_outcome_helper.h>
#include <plugins/baseline/relation/relation_basis.h>
#include <plugins/baseline/rebuild/candidate_builder.h>
#include <plugins/baseline/rebuild/candidate_validator.h>
#include <plugins/baseline/rebuild/formal_model_trainer.h>
#include <plugins/baseline/solver/solver_backend.h>

using namespace flowsql;
using namespace flowsql::baseline;

namespace {

bool NearlyEqual(double lhs, double rhs, double eps = 1e-9) {
    return std::fabs(lhs - rhs) <= eps;
}

constexpr double kPi = 3.14159265358979323846;

int64_t UtcBucket(int year, int month, int day, int hour, int minute, int second, int64_t delta) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    const std::time_t epoch = timegm(&tm);
    return static_cast<int64_t>(epoch) / delta;
}

BaselineTaskSpec BuildEventTask() {
    BaselineTaskSpec task;
    task.name = "bytes_total";
    task.key = "svc-a";
    task.feature = "bytes_total";
    task.feature_type = "value_basic";
    task.feature_profile = "traffic";
    task.delta = 60;
    task.tz = "Asia/Shanghai";
    return task;
}

ValueFeatureProfile BuildValueT1aProfile() {
    ValueFeatureProfile profile;
    profile.feature_type = "value_basic";
    profile.feature_profile = "traffic";
    profile.transform_name = "log1p";
    return profile;
}

std::shared_ptr<ValueFormalModel> BuildConstantValueFormalModel(double beta0,
                                                                double sigma_ref,
                                                                uint64_t version,
                                                                const char* transform_name) {
    auto model = std::make_shared<ValueFormalModel>();
    model->metadata.kind = FormalModelKind::kValueBaseline;
    model->metadata.model_version = version;
    model->readiness = ModelReadiness::kCoreNoMonthReady;
    model->transform_name = transform_name ? transform_name : "identity";
    model->delta = 60;
    model->tz = "UTC";
    model->train_start = 100;
    model->train_end = 101;
    model->confidence_base_at_train = 1.0;
    model->sigma_ref = sigma_ref;
    model->core_block.beta0 = beta0;
    return model;
}

RatioFeatureProfile BuildRatioProfile() {
    RatioFeatureProfile profile;
    profile.feature_type = "ratio";
    profile.feature_profile = "rate_core";
    profile.d_min_train = 50;
    profile.d_score_min = 25;
    profile.d_shift_min = 100;
    profile.kappa_den = 50.0;
    profile.s_prior = 2.0;
    profile.phi_over = 1.5;
    return profile;
}

DetectorResult BuildFusionDetectorResult(const char* key,
                                         const char* feature,
                                         BaselineDirection direction,
                                         double normalized_score,
                                         double confidence,
                                         uint32_t persistence) {
    DetectorResult result;
    result.status = error::OK;
    result.key = BaselineStringRef{key, static_cast<uint32_t>(std::strlen(key))};
    result.feature = BaselineStringRef{feature, static_cast<uint32_t>(std::strlen(feature))};
    result.ts = 10;
    result.normalized_score = normalized_score;
    result.confidence = confidence;
    result.persistence = persistence;
    result.direction = direction;
    result.reason_code =
        direction == BaselineDirection::kDown ? BaselineReasonCode::kDrop
                                              : BaselineReasonCode::kSpike;
    return result;
}

void TestProfileConfigDefaultsAndDerivedValues() {
    std::printf("[TEST] Profile config defaults and derived values...\n");

    const SharedProfileConfig shared = DefaultSharedProfileConfig();
    assert(shared.k_day == 4);
    assert(shared.k_week == 3);
    assert(shared.dme_max == 7);
    assert(shared.m_month_enable == 4);
    assert(NearlyEqual(shared.month_cov_min, 0.8));
    assert(NearlyEqual(shared.z_warn, 3.0));
    assert(NearlyEqual(shared.z_crit, 5.0));
    assert(NearlyEqual(shared.drift.lambda_mem, 0.9));
    assert(shared.drift.m_shift == 3);

    ValueSampledProfileConfig cont_tail;
    assert(GetValueSampledProfileConfig("cont_tail", &cont_tail));
    assert(cont_tail.n_train_min == 100);
    assert(cont_tail.n_score_min() == 50);
    assert(cont_tail.n_shift_min() == 200);
    assert(NearlyEqual(cont_tail.kappa_sample(), 100.0));
    assert(cont_tail.transform_name_override == "log1p");

    RatioProfileConfig ratio_bursty;
    assert(GetRatioProfileConfig("ratio_bursty", &ratio_bursty));
    assert(ratio_bursty.d_min_train == 100);
    assert(ratio_bursty.d_score_min() == 50);
    assert(ratio_bursty.d_shift_min() == 200);
    assert(NearlyEqual(ratio_bursty.kappa_den(), 100.0));
    assert(NearlyEqual(ratio_bursty.s_prior, 4.0));
    assert(NearlyEqual(ratio_bursty.phi_over, 2.0));

    const RatioPriorConfig prior = ComputeRatioPrior(ratio_bursty, 10.0, 100.0);
    assert(NearlyEqual(prior.m0, 0.1));
    assert(NearlyEqual(prior.alpha0, 0.4));
    assert(NearlyEqual(prior.beta0, 3.6));

    std::printf("[PASS] Profile config defaults and derived values\n");
}

void TestRuntimeConfigYamlLoad() {
    std::printf("[TEST] Runtime config YAML load...\n");

    const std::string file_path = "/tmp/flowsql-baseline-runtime-config.yaml";
    std::ofstream out(file_path);
    out << R"(
baseline:
  parser:
    tz_default: "UTC"
  shared_profile_config:
    k_day: 3
    z_warn: 2.8
    z_crit: 4.8
  value_sampled_profiles:
    cont_core:
      n_train_min: 60
      transform_name_override: "log1p"
    cont_tail:
      n_train_min: 120
      transform_name_override: "log1p"
  ratio_profiles:
    global:
      eps_logit: 1.0e-3
      m_floor: 1.0e-3
      v_floor: 0.3
    rate_core:
      d_min_train: 70
      s_prior: 2.5
      phi_over: 1.6
    ratio_bursty:
      d_min_train: 140
      s_prior: 4.5
      phi_over: 2.2
  solver_constants:
    solver_name: "weighted_huber_ridge_irls"
    c_huber: 1.8
    s_min_fit: 1.0e-3
    max_iter_fit: 20
    tol_obj_rel: 1.0e-4
    tol_beta_inf: 1.0e-5
    cond_max: 1.0e8
  runtime_and_rebuild_constants:
    runtime_state_prune:
      idle_bucket_gap: 2048
      prune_scan_limit: 16
    candidate_builder:
      min_train_point_count: 3
    candidate_validator:
      huber_delta: 2.0
      shadow_alpha: 0.3
      ratio_variance_floor: 0.4
      switch_loss_abs_tol: 1.0e-10
    relation_rebuild:
      min_replay_for_holdout: 5
      switch_validation_tail: 10
  scoring_and_confidence_constants:
    score_warn: 2.5
    score_crit: 4.5
    confidence_formal_base: 0.9
    confidence_source_base: 0.7
    confidence_shadow_base: 0.55
    value_shadow_confidence_cap: 0.75
    ratio_shadow_confidence_cap: 0.70
    value_shadow_sigma_scale: 1.7
    ratio_shadow_score_scale: 1.8
  fusion_constants:
    key_risk_fusion:
      fuse_persistence_window: 4.0
      window_limit: 3
    relation_pattern_fusion:
      lambda_sup: 0.6
      lambda_opp: 0.4
      fuse_persistence_window: 4.5
)";
    out.close();

    std::string err;
    assert(LoadBaselineRuntimeConfigFromYaml(file_path, true, &err) == 0);
    assert(BaselineDefaultTimezone() == "UTC");

    const SharedProfileConfig shared = DefaultSharedProfileConfig();
    assert(shared.k_day == 3);
    assert(NearlyEqual(shared.z_warn, 2.8));
    assert(NearlyEqual(shared.z_crit, 4.8));

    ValueSampledProfileConfig sampled_profile;
    assert(GetValueSampledProfileConfig("cont_core", &sampled_profile));
    assert(sampled_profile.n_train_min == 60);

    RatioProfileConfig ratio_profile;
    assert(GetRatioProfileConfig("rate_core", &ratio_profile));
    assert(ratio_profile.d_min_train == 70);
    assert(NearlyEqual(ratio_profile.s_prior, 2.5));
    assert(NearlyEqual(ratio_profile.phi_over, 1.6));
    assert(NearlyEqual(ratio_profile.eps_logit, 1.0e-3));
    assert(NearlyEqual(ratio_profile.m_floor, 1.0e-3));
    assert(NearlyEqual(ratio_profile.v_floor, 0.3));

    assert(RuntimeIdlePruneBucketGap() == 2048);
    assert(RuntimeIdlePruneScanLimit() == 16);
    assert(CandidateMinTrainPointCount() == 3);
    assert(NearlyEqual(CandidateHuberDelta(), 2.0));
    assert(NearlyEqual(CandidateShadowAlpha(), 0.3));
    assert(NearlyEqual(CandidateRatioVarianceFloor(), 0.4));
    assert(NearlyEqual(CandidateSwitchLossAbsTol(), 1.0e-10));
    assert(RelationMinReplayForHoldout() == 5);
    assert(RelationSwitchValidationTail() == 10);

    assert(NearlyEqual(ScoreWarn(), 2.5));
    assert(NearlyEqual(ScoreCrit(), 4.5));
    assert(NearlyEqual(ConfidenceFormalBase(), 0.9));
    assert(NearlyEqual(ConfidenceSourceBase(), 0.7));
    assert(NearlyEqual(ConfidenceShadowBase(), 0.55));
    assert(NearlyEqual(ValueShadowConfidenceCap(), 0.75));
    assert(NearlyEqual(RatioShadowConfidenceCap(), 0.70));
    assert(NearlyEqual(ValueShadowSigmaScale(), 1.7));
    assert(NearlyEqual(RatioShadowScoreScale(), 1.8));

    assert(NearlyEqual(KeyFusionPersistenceWindow(), 4.0));
    assert(KeyFusionWindowLimit() == 3);
    assert(NearlyEqual(RelationPatternLambdaSup(), 0.6));
    assert(NearlyEqual(RelationPatternLambdaOpp(), 0.4));
    assert(NearlyEqual(RelationPatternPersistenceWindow(), 4.5));

    const BlockSolverConfig solver = DefaultBlockSolverConfig();
    assert(solver.max_iter_fit == 20);
    assert(NearlyEqual(solver.c_huber, 1.8));

    ResetBaselineRuntimeConfig();

    assert(BaselineDefaultTimezone() == "Asia/Shanghai");
    assert(RuntimeIdlePruneBucketGap() == 4096);
    assert(RuntimeIdlePruneScanLimit() == 32);
    assert(CandidateMinTrainPointCount() == 2);
    assert(NearlyEqual(ScoreWarn(), 3.0));
    assert(NearlyEqual(ScoreCrit(), 5.0));
    assert(NearlyEqual(KeyFusionPersistenceWindow(), 3.0));
    assert(KeyFusionWindowLimit() == 2);
    assert(RelationMinReplayForHoldout() == 3);
    assert(RelationSwitchValidationTail() == 16);

    std::remove(file_path.c_str());
    std::printf("[PASS] Runtime config YAML load\n");
}

void TestRuntimeConfigYamlValidation() {
    std::printf("[TEST] Runtime config YAML validation...\n");

    const std::string invalid_score_path = "/tmp/flowsql-baseline-runtime-config-invalid.yaml";
    std::ofstream invalid_score(invalid_score_path);
    invalid_score << R"(
baseline:
  scoring_and_confidence_constants:
    score_warn: 5.0
    score_crit: 4.0
)";
    invalid_score.close();

    std::string err;
    assert(LoadBaselineRuntimeConfigFromYaml(invalid_score_path, true, &err) != 0);
    std::remove(invalid_score_path.c_str());

    const std::string unknown_field_path = "/tmp/flowsql-baseline-runtime-config-unknown.yaml";
    std::ofstream unknown_field(unknown_field_path);
    unknown_field << R"(
baseline:
  scoring_and_confidence_constants:
    score_warn: 3.0
    score_crit: 5.0
    unknown_field: 1
)";
    unknown_field.close();
    err.clear();
    assert(LoadBaselineRuntimeConfigFromYaml(unknown_field_path, true, &err) != 0);

    const std::string invalid_relation_path = "/tmp/flowsql-baseline-runtime-config-relation.yaml";
    std::ofstream invalid_relation(invalid_relation_path);
    invalid_relation << R"(
baseline:
  runtime_and_rebuild_constants:
    relation_rebuild:
      min_replay_for_holdout: 0
      switch_validation_tail: 16
)";
    invalid_relation.close();
    err.clear();
    assert(LoadBaselineRuntimeConfigFromYaml(invalid_relation_path, true, &err) != 0);

    ResetBaselineRuntimeConfig();
    std::remove(unknown_field_path.c_str());
    std::remove(invalid_relation_path.c_str());
    std::printf("[PASS] Runtime config YAML validation\n");
}

void TestCalendarFeatureHelper() {
    std::printf("[TEST] Calendar feature helper...\n");

    constexpr int64_t delta = 60;
    const int64_t utc_midnight = UtcBucket(2026, 1, 1, 0, 0, 0, delta);
    const int64_t utc_six = UtcBucket(2026, 1, 1, 6, 0, 0, delta);
    assert(NearlyEqual(PhaseDayLocal(utc_midnight, delta, "UTC"), 0.0));
    assert(NearlyEqual(PhaseDayLocal(utc_six, delta, "UTC"), 0.25));

    const int64_t monday = UtcBucket(2026, 1, 5, 0, 0, 0, delta);
    assert(NearlyEqual(PhaseWeekLocal(monday, delta, "UTC"), 0.0));

    const int64_t shanghai_midnight = UtcBucket(2026, 1, 1, 16, 0, 0, delta);
    assert(NearlyEqual(PhaseDayLocal(shanghai_midnight, delta, "Asia/Shanghai"), 0.0));
    assert(DayOfMonthLocal(shanghai_midnight, delta, "Asia/Shanghai") == 2);

    const int64_t jan_30 = UtcBucket(2026, 1, 30, 12, 0, 0, delta);
    const int64_t jan_23 = UtcBucket(2026, 1, 23, 12, 0, 0, delta);
    assert(DaysToMonthEndLocal(jan_30, delta, "UTC") == 1);
    assert(IsLastWeekdayOfMonthLocal(jan_30, delta, "UTC"));
    assert(!IsLastWeekdayOfMonthLocal(jan_23, delta, "UTC"));

    const int64_t dst_after_jump = UtcBucket(2026, 3, 8, 7, 0, 0, delta);
    assert(NearlyEqual(PhaseDayLocal(dst_after_jump, delta, "America/New_York"), 3.0 / 24.0));

    std::printf("[PASS] Calendar feature helper\n");
}

void TestEventCalendarMatcherScopeAndOverlap() {
    std::printf("[TEST] Event calendar matcher scope and overlap...\n");

    BaselineTaskSpec task = BuildEventTask();
    EventCalendarSpec calendar;
    calendar.calendar_id = "ops";
    calendar.calendar_version = "v1";

    EventCalendarEntry global;
    global.event_code = "deploy";
    global.scope_type = "global";
    global.alignment_mode = "absolute_utc";
    global.start_ts = task.delta * 100 + 50;
    global.end_ts = task.delta * 101 + 10;
    calendar.entries.push_back(global);

    EventCalendarEntry feature = global;
    feature.event_code = "feature_hot";
    feature.scope_type = "feature";
    feature.feature = "bytes_total";
    calendar.entries.push_back(feature);

    EventCalendarEntry wrong_feature = global;
    wrong_feature.event_code = "wrong_feature";
    wrong_feature.scope_type = "feature";
    wrong_feature.feature = "pps";
    calendar.entries.push_back(wrong_feature);

    EventCalendarEntry key_feature = global;
    key_feature.event_code = "billing_close";
    key_feature.scope_type = "key_feature";
    key_feature.feature = "bytes_total";
    key_feature.key = "svc-a";
    key_feature.alignment_mode = "local_wall_clock";
    key_feature.tz = "Asia/Shanghai";
    calendar.entries.push_back(key_feature);

    EventCalendarEntry disabled = global;
    disabled.event_code = "disabled";
    disabled.enabled = false;
    calendar.entries.push_back(disabled);

    CompiledEventCalendar compiled;
    std::string err;
    assert(CompileEventCalendar(calendar, task, &compiled, &err) == 0);
    assert(compiled.calendar_id == "ops");
    assert(compiled.calendar_version == "v1");
    assert(compiled.enabled_event_codes.size() == 4);

    const std::vector<std::string> events = ResolveBucketEvents(compiled, task, 100);
    assert(events.size() == 3);
    assert(events[0] == "deploy");
    assert(events[1] == "feature_hot");
    assert(events[2] == "billing_close");

    std::vector<double> row(compiled.enabled_event_codes.size(), -1.0);
    assert(BuildEventIndicatorRow(compiled, task, 100, row.data(), row.size()) == 0);
    assert(NearlyEqual(row[0], 1.0));
    assert(NearlyEqual(row[1], 1.0));
    assert(NearlyEqual(row[2], 0.0));
    assert(NearlyEqual(row[3], 1.0));

    const std::vector<std::string> no_events = ResolveBucketEvents(compiled, task, 99);
    assert(no_events.empty());

    std::printf("[PASS] Event calendar matcher scope and overlap\n");
}

void TestConfigParserTimezoneDefault() {
    std::printf("[TEST] Config parser timezone default...\n");

    BaselineTaskSpec value_spec;
    std::string err;
    const int value_rc = ConfigParser::ParseValueTask(
        R"({"name":"avg_rtt","key":"service","feature":"avg_rtt","feature_type":"value_sampled","feature_profile":"cont_core","delta":60})",
        &value_spec,
        &err);
    assert(value_rc == error::OK);
    assert(value_spec.tz == "Asia/Shanghai");

    RelationTaskCreateSpec relation_spec;
    err.clear();
    const int relation_rc = ConfigParser::ParseRelationTask(
        R"({"name":"client_group_mix","feature_base":"client_group_mix","group_space_id":"client_group","metric_set_id":"net_metrics","metrics":["conn_count"],"encode_type":"exact_sparse","support_policy":{"k_support":8,"min_hist_share":0.005,"min_active_ratio":0.2},"summary_policy":{"k_head":2,"k_stable":2},"delta":60})",
        &relation_spec,
        &err);
    assert(relation_rc == error::OK);
    assert(relation_spec.clock_spec.tz == "Asia/Shanghai");

    std::printf("[PASS] Config parser timezone default\n");
}

void TestEventCalendarLocalWallClockInheritsTaskTimezone() {
    std::printf("[TEST] Event calendar local wall clock inherits task timezone...\n");

    BaselineTaskSpec task = BuildEventTask();
    EventCalendarSpec calendar;
    calendar.calendar_id = "ops";
    calendar.calendar_version = "v1";

    EventCalendarEntry local_wall_clock;
    local_wall_clock.event_code = "month_close";
    local_wall_clock.scope_type = "global";
    local_wall_clock.alignment_mode = "local_wall_clock";
    local_wall_clock.start_ts = task.delta * 100 + 50;
    local_wall_clock.end_ts = task.delta * 101 + 10;
    calendar.entries.push_back(local_wall_clock);

    CompiledEventCalendar compiled;
    std::string err;
    assert(CompileEventCalendar(calendar, task, &compiled, &err) == error::OK);
    assert(compiled.entries.size() == 1);
    assert(compiled.entries[0].tz.empty());

    const std::vector<std::string> events = ResolveBucketEvents(compiled, task, 100);
    assert(events.size() == 1);
    assert(events[0] == "month_close");

    std::printf("[PASS] Event calendar local wall clock inherits task timezone\n");
}

void TestReadinessHelper() {
    std::printf("[TEST] Readiness helper...\n");

    const SharedProfileConfig config = DefaultSharedProfileConfig();
    ReadinessState online;
    UpdateCoverageStats(&online, 10, true);
    UpdateCoverageStats(&online, 11, false);
    UpdateCoverageStats(&online, 12, true);
    UpdateCoverageStats(&online, 13, true);
    online.coverage_stats.month_count = 4;

    assert(online.coverage_stats.valid_bucket_count == 3);
    assert(online.coverage_stats.total_bucket_span == 4);
    assert(NearlyEqual(online.coverage_stats.coverage, 0.75));
    assert(!EvaluateMonthPosEligibility(online, config));

    online.coverage_stats.valid_bucket_count = 8;
    online.coverage_stats.total_bucket_span = 10;
    online.coverage_stats.coverage = 0.8;
    assert(EvaluateMonthPosEligibility(online, config));

    RefreshOnlineReadiness(&online, config, ModelReadiness::kMonthposReady, BaselineSourceKind::kSelf);
    assert(online.monthpos_enabled);
    assert(!online.coverage_degraded);
    assert(NearlyEqual(online.confidence_base, 1.0));

    TrainingCoverageStats train;
    train.valid_bucket_count = 18;
    train.total_bucket_span = 20;
    train.month_count = 4;
    train.first_bucket_id = 100;
    train.last_bucket_id = 119;

    const ReadinessState train_readiness = BuildTrainReadiness(train, config);
    assert(train_readiness.monthpos_enabled);
    assert(train_readiness.readiness == ModelReadiness::kMonthposReady);
    assert(NearlyEqual(train_readiness.confidence_base, 1.0));

    std::printf("[PASS] Readiness helper\n");
}

void TestRelationPatternFusionSupportEscape() {
    std::printf("[TEST] Relation pattern fusion support_escape...\n");

    RelationPatternFusionInput input;
    input.key = "svc-a";
    input.bucket_id = 10;
    input.feature_base = "client_group_mix";
    input.singles = {
        FusionSingleContribution{
            1,
            "conn_count",
            RelationSummaryKind::kOutOfSupportShare,
            BuildFusionDetectorResult("svc-a",
                                      "client_group_mix_conn_count_out_of_support_share",
                                      BaselineDirection::kUp,
                                      0.90,
                                      1.00,
                                      3)},
        FusionSingleContribution{
            2,
            "conn_count",
            RelationSummaryKind::kEntropyShannon,
            BuildFusionDetectorResult("svc-a",
                                      "client_group_mix_conn_count_entropy_shannon",
                                      BaselineDirection::kUp,
                                      0.60,
                                      1.00,
                                      3)},
        FusionSingleContribution{
            3,
            "conn_count",
            RelationSummaryKind::kStableHeadCoverage,
            BuildFusionDetectorResult("svc-a",
                                      "client_group_mix_conn_count_stable_head_coverage",
                                      BaselineDirection::kDown,
                                      0.40,
                                      1.00,
                                      3)},
        FusionSingleContribution{
            4,
            "conn_count",
            RelationSummaryKind::kTop1Share,
            BuildFusionDetectorResult("svc-a",
                                      "client_group_mix_conn_count_top1_share",
                                      BaselineDirection::kUp,
                                      0.10,
                                      1.00,
                                      3)},
    };

    RelationPatternFusionOutput output;
    assert(RelationPatternFusion::Compute(input, &output) == error::OK);
    assert(output.fusion_result.available);
    assert(output.fusion_result.risk > 0.0);
    assert(!output.pattern_contributions.empty());

    const auto pattern_it = std::find_if(output.pattern_contributions.begin(),
                                         output.pattern_contributions.end(),
                                         [](const FusionPatternContribution& item) {
                                             return item.pattern == PatternCode::kSupportEscape;
                                         });
    assert(pattern_it != output.pattern_contributions.end());
    assert(pattern_it->projection.pattern == "support_escape");
    assert(pattern_it->projection.feature_base == "client_group_mix");
    assert(pattern_it->projection.score_pattern > 0.9);
    assert(pattern_it->projection.metrics_hit_count == 1);
    assert(pattern_it->projection.metrics_hit[0] == "conn_count");
    assert(pattern_it->projection.supporting_feature_count >= 2);
    assert(output.fusion_result.dominant_pattern_count >= 1);
    assert(output.fusion_result.dominant_patterns[0].pattern == "support_escape");

    std::printf("[PASS] Relation pattern fusion support_escape\n");
}

void TestRelationPatternFusionRemainingPatterns() {
    std::printf("[TEST] Relation pattern fusion remaining patterns...\n");

    {
        RelationPatternFusionInput input;
        input.key = "svc-head";
        input.bucket_id = 11;
        input.feature_base = "client_group_mix";
        input.singles = {
            FusionSingleContribution{1,
                                     "conn_count",
                                     RelationSummaryKind::kTop1Share,
                                     BuildFusionDetectorResult("svc-head",
                                                               "client_group_mix_conn_count_top1_share",
                                                               BaselineDirection::kUp,
                                                               0.90,
                                                               1.00,
                                                               3)},
            FusionSingleContribution{2,
                                     "conn_count",
                                     RelationSummaryKind::kHeadKShare,
                                     BuildFusionDetectorResult("svc-head",
                                                               "client_group_mix_conn_count_headK_share",
                                                               BaselineDirection::kUp,
                                                               0.70,
                                                               1.00,
                                                               3)},
            FusionSingleContribution{3,
                                     "conn_count",
                                     RelationSummaryKind::kEntropyShannon,
                                     BuildFusionDetectorResult("svc-head",
                                                               "client_group_mix_conn_count_entropy_shannon",
                                                               BaselineDirection::kDown,
                                                               0.60,
                                                               1.00,
                                                               3)},
        };

        RelationPatternFusionOutput output;
        assert(RelationPatternFusion::Compute(input, &output) == error::OK);
        const auto pattern_it = std::find_if(output.pattern_contributions.begin(),
                                             output.pattern_contributions.end(),
                                             [](const FusionPatternContribution& item) {
                                                 return item.pattern ==
                                                        PatternCode::kHeadConcentration;
                                             });
        assert(pattern_it != output.pattern_contributions.end());
        assert(pattern_it->projection.pattern == "head_concentration");
        assert(pattern_it->projection.score_pattern > 0.9);
    }

    {
        RelationPatternFusionInput input;
        input.key = "svc-dilution";
        input.bucket_id = 12;
        input.feature_base = "client_group_mix";
        input.singles = {
            FusionSingleContribution{1,
                                     "conn_count",
                                     RelationSummaryKind::kStableHeadCoverage,
                                     BuildFusionDetectorResult("svc-dilution",
                                                               "client_group_mix_conn_count_stable_head_coverage",
                                                               BaselineDirection::kDown,
                                                               0.90,
                                                               1.00,
                                                               3)},
            FusionSingleContribution{2,
                                     "conn_count",
                                     RelationSummaryKind::kOutOfSupportShare,
                                     BuildFusionDetectorResult("svc-dilution",
                                                               "client_group_mix_conn_count_out_of_support_share",
                                                               BaselineDirection::kUp,
                                                               0.70,
                                                               1.00,
                                                               3)},
            FusionSingleContribution{3,
                                     "conn_count",
                                     RelationSummaryKind::kEntropyShannon,
                                     BuildFusionDetectorResult("svc-dilution",
                                                               "client_group_mix_conn_count_entropy_shannon",
                                                               BaselineDirection::kUp,
                                                               0.60,
                                                               1.00,
                                                               3)},
        };

        RelationPatternFusionOutput output;
        assert(RelationPatternFusion::Compute(input, &output) == error::OK);
        const auto pattern_it = std::find_if(output.pattern_contributions.begin(),
                                             output.pattern_contributions.end(),
                                             [](const FusionPatternContribution& item) {
                                                 return item.pattern ==
                                                        PatternCode::kLegacyHeadDilution;
                                             });
        assert(pattern_it != output.pattern_contributions.end());
        assert(pattern_it->projection.pattern == "legacy_head_dilution");
        assert(pattern_it->projection.score_pattern > 0.9);
    }

    {
        RelationPatternFusionInput input;
        input.key = "svc-mix";
        input.bucket_id = 13;
        input.feature_base = "client_group_mix";
        input.singles = {
            FusionSingleContribution{1,
                                     "conn_count",
                                     RelationSummaryKind::kStableHeadMixDrift,
                                     BuildFusionDetectorResult("svc-mix",
                                                               "client_group_mix_conn_count_stable_head_mix_drift",
                                                               BaselineDirection::kUp,
                                                               0.90,
                                                               1.00,
                                                               3)},
        };

        RelationPatternFusionOutput output;
        assert(RelationPatternFusion::Compute(input, &output) == error::OK);
        const auto pattern_it = std::find_if(output.pattern_contributions.begin(),
                                             output.pattern_contributions.end(),
                                             [](const FusionPatternContribution& item) {
                                                 return item.pattern ==
                                                        PatternCode::kStableHeadMixShift;
                                             });
        assert(pattern_it != output.pattern_contributions.end());
        assert(pattern_it->projection.pattern == "stable_head_mix_shift");
        assert(pattern_it->projection.score_pattern > 0.8);
    }

    std::printf("[PASS] Relation pattern fusion remaining patterns\n");
}

void TestCandidateBuilderRelationBasisViews() {
    std::printf("[TEST] Candidate builder relation basis views...\n");

    RelationBasisBuildInput input;
    input.basis_version = 4;
    input.feature_base = "client_group_mix";
    input.metric_name = "conn_count";
    input.group_space_id = "client_group";
    input.group_space_version = "v1";
    input.support_policy.k_support = 3;
    input.support_policy.min_hist_share = 0.10;
    input.support_policy.min_active_ratio = 0.20;
    input.summary_policy.k_head = 2;
    input.summary_policy.k_stable = 2;
    input.valid_bucket_count = 10;
    input.group_stats = {
        {11, 50.0, 10},
        {12, 30.0, 8},
        {13, 20.0, 5},
    };

    RelationServiceBasis incumbent_basis;
    incumbent_basis.basis_version = 3;
    incumbent_basis.feature_base = "client_group_mix";
    incumbent_basis.metric_name = "conn_count";
    incumbent_basis.group_space_id = "client_group";
    incumbent_basis.group_space_version = "v1";
    incumbent_basis.k_head = 2;
    incumbent_basis.support_explicit = {21, 22, 23};
    incumbent_basis.stable_head = {21, 22};
    incumbent_basis.head_proto_q = {0.7, 0.3};

    RelationTaskSpec compatible_spec;
    compatible_spec.feature_base = "client_group_mix";
    compatible_spec.group_space_id = "client_group";
    compatible_spec.group_space_version = "v1";

    RelationMetricCandidateBuildResult compatible_result;
    assert(CandidateBuilder::BuildRelationMetricBases(input,
                                                      &incumbent_basis,
                                                      compatible_spec,
                                                      &compatible_result) ==
           CandidateBuildStatus::kTrained);
    assert(compatible_result.lineage_compatibility ==
           RelationLineageCompatibility::kIdentical);
    assert(compatible_result.candidate_service_basis.group_space_version == "v1");
    assert(compatible_result.candidate_eval_basis.basis.group_space_version == "v1");
    assert(compatible_result.candidate_eval_basis.basis.support_explicit.size() == 3);

    RelationBasisBuildInput new_lineage_input = input;
    new_lineage_input.group_space_version = "v3";
    RelationTaskSpec new_lineage_spec = compatible_spec;
    new_lineage_spec.group_space_version = "v3";

    RelationMetricCandidateBuildResult new_lineage_result;
    assert(CandidateBuilder::BuildRelationMetricBases(new_lineage_input,
                                                      &incumbent_basis,
                                                      new_lineage_spec,
                                                      &new_lineage_result) ==
           CandidateBuildStatus::kTrained);
    assert(new_lineage_result.lineage_compatibility ==
           RelationLineageCompatibility::kNewLineage);
    assert(new_lineage_result.candidate_eval_basis.has_incumbent == true);
    assert(new_lineage_result.candidate_eval_basis.basis.support_explicit.empty());
    assert(new_lineage_result.candidate_service_basis.group_space_version == "v3");

    std::printf("[PASS] Candidate builder relation basis views\n");
}

void TestCandidateValidatorRelationAggregate() {
    std::printf("[TEST] Candidate validator relation aggregate...\n");

    const CandidateValidationResult pass_result =
        CandidateValidator::ValidateRelationAggregate(0.20, 0.21, 2);
    assert(pass_result.status == CandidateValidationStatus::kPassed);
    assert(pass_result.pass);
    assert(pass_result.validation_count == 2);
    assert(NearlyEqual(pass_result.candidate_loss, 0.10));
    assert(NearlyEqual(pass_result.incumbent_loss, 0.105));

    const CandidateValidationResult fail_result =
        CandidateValidator::ValidateRelationAggregate(0.30, 0.20, 2);
    assert(fail_result.status == CandidateValidationStatus::kFailed);
    assert(!fail_result.pass);
    assert(fail_result.validation_count == 2);

    const CandidateValidationResult empty_result =
        CandidateValidator::ValidateRelationAggregate(0.0, 0.0, 0);
    assert(empty_result.status == CandidateValidationStatus::kInsufficientHoldout);
    assert(!empty_result.pass);

    std::printf("[PASS] Candidate validator relation aggregate\n");
}

void TestFormalModelSchemaAndPredictor() {
    std::printf("[TEST] Formal model schema and predictor...\n");

    constexpr int64_t delta = 60;
    const int64_t bucket_id = UtcBucket(2026, 1, 1, 6, 0, 0, delta);
    BaselineTaskSpec task = BuildEventTask();
    task.key = "svc-a";
    task.feature = "bytes_total";
    task.delta = delta;
    task.tz = "UTC";

    EventCalendarSpec calendar;
    calendar.calendar_id = "ops";
    calendar.calendar_version = "v1";
    EventCalendarEntry deploy;
    deploy.event_code = "deploy";
    deploy.scope_type = "key_feature";
    deploy.alignment_mode = "absolute_utc";
    deploy.key = "svc-a";
    deploy.feature = "bytes_total";
    deploy.start_ts = bucket_id * delta;
    deploy.end_ts = bucket_id * delta + delta;
    calendar.entries.push_back(deploy);

    CompiledEventCalendar compiled;
    std::string err;
    assert(CompileEventCalendar(calendar, task, &compiled, &err) == 0);

    ValueFormalModel value_model;
    value_model.metadata.kind = FormalModelKind::kValueBaseline;
    value_model.metadata.model_version = 3;
    value_model.metadata.calendar_id = "ops";
    value_model.metadata.calendar_version = "v1";
    value_model.readiness = ModelReadiness::kMonthposReady;
    value_model.transform_name = "log1p";
    value_model.solver_name = "weighted_huber_ridge_irls";
    value_model.fit_strategy = "stage_fit";
    value_model.delta = delta;
    value_model.tz = "UTC";
    value_model.feature_profile = "traffic";
    value_model.train_start = bucket_id - 10;
    value_model.train_end = bucket_id - 1;
    value_model.confidence_base_at_train = 1.0;
    value_model.sigma_ref = 0.5;
    value_model.core_block.beta0 = 10.0;
    value_model.core_block.trend_k = 0.5;
    value_model.core_block.day_sin = {1.0};
    value_model.monthpos_block.enabled = true;
    value_model.monthpos_block.dom_coeff[0] = 3.0;
    value_model.event_block.enabled = true;
    value_model.event_block.calendar_id = "ops";
    value_model.event_block.calendar_version = "v1";
    value_model.event_block.active_event_codes = {"deploy"};
    value_model.event_block.coeff = {2.0};
    value_model.fit_summary.push_back(FitBlockDigest{"core", "ok", 10, 0.0, 1.0});

    FormalPredictContext predict_context;
    predict_context.task_spec = &task;
    predict_context.event_calendar = &compiled;
    predict_context.bucket_id = bucket_id;

    FormalPrediction value_prediction;
    assert(PredictFormalModel(&value_model, predict_context, &value_prediction) == 0);
    assert(value_prediction.ready);
    assert(value_prediction.model_kind == FormalModelKind::kValueBaseline);
    assert(value_prediction.event_status == EventCalendarStatus::kEnabled);
    assert(value_prediction.event_enabled);
    assert(NearlyEqual(value_prediction.sigma_ref, 0.5));
    assert(NearlyEqual(value_prediction.confidence_base, 1.0));
    assert(value_prediction.readiness == ModelReadiness::kMonthposReady);
    assert(NearlyEqual(value_prediction.value, 10.0 + 5.0 + 1.0 + 3.0 + 2.0));

    RatioFormalModel ratio_model;
    ratio_model.metadata.kind = FormalModelKind::kRatioBaseline;
    ratio_model.metadata.model_version = 4;
    ratio_model.metadata.calendar_id = "ops";
    ratio_model.metadata.calendar_version = "v1";
    ratio_model.readiness = ModelReadiness::kCoreNoMonthReady;
    ratio_model.transform_name = "logit";
    ratio_model.delta = delta;
    ratio_model.tz = "UTC";
    ratio_model.feature_profile = "rate_core";
    ratio_model.train_start = bucket_id;
    ratio_model.train_end = bucket_id;
    ratio_model.confidence_base_at_train = 0.8;
    ratio_model.core_block.beta0 = 0.0;
    ratio_model.event_block.enabled = true;
    ratio_model.event_block.calendar_id = "ops";
    ratio_model.event_block.calendar_version = "v1";
    ratio_model.event_block.active_event_codes = {"deploy"};
    ratio_model.event_block.coeff = {std::log(3.0)};

    FormalPrediction ratio_prediction;
    assert(PredictFormalModel(&ratio_model, predict_context, &ratio_prediction) == 0);
    assert(ratio_prediction.ready);
    assert(ratio_prediction.model_kind == FormalModelKind::kRatioBaseline);
    assert(ratio_prediction.event_enabled);
    assert(NearlyEqual(ratio_prediction.confidence_base, 0.8));
    assert(NearlyEqual(ratio_prediction.value, 0.75));

    std::printf("[PASS] Formal model schema and predictor\n");
}

void TestWeightedHuberRidgeBlockSolver() {
    std::printf("[TEST] Weighted Huber ridge block solver...\n");

    BlockFitSpec spec;
    spec.block_name = "core";
    spec.row_count = 10;
    spec.col_count = 2;
    spec.y_target = {1.0, 3.0, 5.0, 7.0, 9.0, 11.0, 13.0, 15.0, 17.0, 31.0};
    spec.x_matrix = {
        1.0, 0.0,
        1.0, 1.0,
        1.0, 2.0,
        1.0, 3.0,
        1.0, 4.0,
        1.0, 5.0,
        1.0, 6.0,
        1.0, 7.0,
        1.0, 8.0,
        1.0, 9.0,
    };
    spec.sample_weight = std::vector<double>(10, 1.0);
    spec.ridge_diag = {0.0, 0.0};
    spec.col_roles = {
        BlockColumnRole::kIntercept,
        BlockColumnRole::kTrend,
    };

    FitBlockResult fit;
    assert(SolverBackend::FitWeightedHuberRidgeBlock(
               spec, DefaultBlockSolverConfig(), &fit) == error::OK);
    assert(fit.status == BlockFitStatus::kOk || fit.status == BlockFitStatus::kDegraded);
    assert(fit.beta.size() == 2);
    assert(std::fabs(fit.beta[0] - 1.0) < 0.5);
    assert(std::fabs(fit.beta[1] - 2.0) < 0.2);
    assert(std::isfinite(fit.objective));
    assert(std::isfinite(fit.condition_est));

    std::printf("[PASS] Weighted Huber ridge block solver\n");
}

void TestFormalModelTrainerStagesForValue() {
    std::printf("[TEST] Formal model trainer staged value fit...\n");

    constexpr int64_t delta = 86400;
    const int64_t start_bucket = UtcBucket(2026, 1, 1, 0, 0, 0, delta);

    BaselineTaskSpec task = BuildEventTask();
    task.delta = delta;
    task.tz = "UTC";
    task.key = "svc-train";
    task.feature = "bytes_total";

    EventCalendarSpec calendar;
    calendar.calendar_id = "ops";
    calendar.calendar_version = "v2";
    EventCalendarEntry deploy;
    deploy.event_code = "deploy";
    deploy.scope_type = "key_feature";
    deploy.alignment_mode = "absolute_utc";
    deploy.key = "svc-train";
    deploy.feature = "bytes_total";

    ValueReplaySeries replay;
    replay.key = "svc-train";
    replay.window.has_data = true;
    replay.window.request_bucket_start = start_bucket;
    replay.window.request_bucket_end = start_bucket + 129;
    replay.window.first_bucket_id = start_bucket;
    replay.window.last_bucket_id = start_bucket + 129;
    replay.window.observation_count = 130;

    for (int i = 0; i < 130; ++i) {
        const int64_t bucket_id = start_bucket + i;
        const int32_t dom = DayOfMonthLocal(bucket_id, delta, "UTC");
        const int32_t dme = DaysToMonthEndLocal(bucket_id, delta, "UTC");
        const bool last_weekday = IsLastWeekdayOfMonthLocal(bucket_id, delta, "UTC");

        double x = 1.0 + 0.002 * static_cast<double>(i);
        x += 0.15 * std::sin(2.0 * kPi * PhaseWeekLocal(bucket_id, delta, "UTC"));
        x += 0.18 * static_cast<double>((dom % 5) - 2);
        if (dme <= 1) x += 0.45;
        if (last_weekday) x += 0.60;
        if ((i >= 40 && i <= 42) || (i >= 85 && i <= 87)) {
            x += 0.80;
            EventCalendarEntry event = deploy;
            event.start_ts = bucket_id * delta;
            event.end_ts = bucket_id * delta + delta;
            calendar.entries.push_back(event);
        }

        replay.points.push_back(ValueReplayPoint{bucket_id, std::exp(x) - 1.0, 0});
    }

    CompiledEventCalendar compiled;
    std::string err;
    assert(CompileEventCalendar(calendar, task, &compiled, &err) == error::OK);

    const ValueFeatureProfile profile = BuildValueT1aProfile();
    ValueFormalTrainResult result;
    ValueFormalTrainInput input;
    input.profile = &profile;
    input.replay = &replay;
    input.train_count = replay.points.size();
    input.model_version = 7;
    input.holdout_count = 0;
    input.train_window = replay.window;
    input.task_spec = &task;
    input.delta = delta;
    input.tz = "UTC";
    input.compiled_event_calendar = &compiled;

    assert(FormalModelTrainer::TrainValue(input, &result) == FormalTrainFailureCode::kNone);
    assert(result.model != nullptr);
    assert(result.model->metadata.model_version == 7);
    assert(result.model->delta == delta);
    assert(result.model->tz == "UTC");
    assert(result.model->monthpos_block.enabled);
    assert(result.model->event_block.enabled);
    assert(result.model->fit_summary.size() == 3);
    assert(result.model->fit_summary[0].status == "ok");
    assert(result.model->fit_summary[1].status == "ok");
    assert(result.model->fit_summary[2].status == "ok");
    bool has_monthpos_signal = false;
    for (double coeff : result.model->monthpos_block.dom_coeff) {
        has_monthpos_signal = has_monthpos_signal || std::fabs(coeff) > 0.05;
    }
    for (double coeff : result.model->monthpos_block.dme_coeff) {
        has_monthpos_signal = has_monthpos_signal || std::fabs(coeff) > 0.05;
    }
    for (double coeff : result.model->monthpos_block.lwd_coeff) {
        has_monthpos_signal = has_monthpos_signal || std::fabs(coeff) > 0.05;
    }
    assert(has_monthpos_signal);
    assert(!result.model->event_block.coeff.empty());
    assert(std::fabs(result.model->event_block.coeff[0]) > 0.05);
    assert(result.model->metadata.calendar_id == "ops");
    assert(result.model->metadata.calendar_version == "v2");

    FormalPredictContext context;
    context.task_spec = &task;
    context.event_calendar = &compiled;
    context.bucket_id = start_bucket + 85;

    FormalPrediction prediction;
    assert(PredictFormalModel(result.model.get(), context, &prediction) == error::OK);
    assert(prediction.ready);
    assert(prediction.event_enabled);
    assert(prediction.value > 1.0);
    assert(prediction.sigma_ref > 0.0);

    std::printf("[PASS] Formal model trainer staged value fit\n");
}

void TestFormalModelTrainerStagesForRatio() {
    std::printf("[TEST] Formal model trainer staged ratio fit...\n");

    constexpr int64_t delta = 86400;
    const int64_t start_bucket = UtcBucket(2026, 1, 1, 0, 0, 0, delta);

    BaselineTaskSpec task;
    task.name = "success_rate";
    task.key = "svc-ratio";
    task.feature = "success_rate";
    task.feature_type = "ratio";
    task.feature_profile = "rate_core";
    task.delta = delta;
    task.tz = "UTC";

    EventCalendarSpec calendar;
    calendar.calendar_id = "ops";
    calendar.calendar_version = "v3";
    EventCalendarEntry deploy;
    deploy.event_code = "deploy";
    deploy.scope_type = "key_feature";
    deploy.alignment_mode = "absolute_utc";
    deploy.key = "svc-ratio";
    deploy.feature = "success_rate";

    RatioReplaySeries replay;
    replay.key = "svc-ratio";
    replay.window.has_data = true;
    replay.window.request_bucket_start = start_bucket;
    replay.window.request_bucket_end = start_bucket + 129;
    replay.window.first_bucket_id = start_bucket;
    replay.window.last_bucket_id = start_bucket + 129;
    replay.window.observation_count = 130;

    for (int i = 0; i < 130; ++i) {
        const int64_t bucket_id = start_bucket + i;
        const int32_t dom = DayOfMonthLocal(bucket_id, delta, "UTC");
        const int32_t dme = DaysToMonthEndLocal(bucket_id, delta, "UTC");
        const bool last_weekday = IsLastWeekdayOfMonthLocal(bucket_id, delta, "UTC");

        double eta = -0.15 + 0.003 * static_cast<double>(i);
        eta += 0.20 * std::sin(2.0 * kPi * PhaseWeekLocal(bucket_id, delta, "UTC"));
        eta += 0.08 * static_cast<double>((dom % 6) - 3);
        if (dme <= 1) eta += 0.35;
        if (last_weekday) eta += 0.25;
        if ((i >= 30 && i <= 32) || (i >= 90 && i <= 92)) {
            eta += 0.50;
            EventCalendarEntry event = deploy;
            event.start_ts = bucket_id * delta;
            event.end_ts = bucket_id * delta + delta;
            calendar.entries.push_back(event);
        }

        const double probability = 1.0 / (1.0 + std::exp(-eta));
        const double denominator = 400.0 + static_cast<double>(i % 17);
        const double numerator = std::round(denominator * probability);
        replay.points.push_back(RatioReplayPoint{bucket_id, numerator, denominator});
    }

    CompiledEventCalendar compiled;
    std::string err;
    assert(CompileEventCalendar(calendar, task, &compiled, &err) == error::OK);

    const RatioFeatureProfile profile = BuildRatioProfile();
    RatioFormalTrainResult result;
    RatioFormalTrainInput input;
    input.profile = &profile;
    input.replay = &replay;
    input.train_count = replay.points.size();
    input.model_version = 8;
    input.holdout_count = 0;
    input.train_window = replay.window;
    input.task_spec = &task;
    input.delta = delta;
    input.tz = "UTC";
    input.compiled_event_calendar = &compiled;

    assert(FormalModelTrainer::TrainRatio(input, &result) == FormalTrainFailureCode::kNone);
    assert(result.model != nullptr);
    assert(result.model->metadata.model_version == 8);
    assert(result.model->delta == delta);
    assert(result.model->tz == "UTC");
    assert(result.model->m0 > 0.0);
    assert(result.model->m0 < 1.0);
    assert(result.model->alpha0 > 0.0);
    assert(result.model->beta0 > 0.0);
    assert(result.model->monthpos_block.enabled);
    assert(result.model->event_block.enabled);
    assert(result.model->readiness == ModelReadiness::kMonthposReady);
    assert(NearlyEqual(result.model->confidence_base_at_train, 1.0));
    assert(result.model->fit_summary.size() == 3);
    assert(result.model->fit_summary[0].status == "ok");
    assert(result.model->fit_summary[1].status == "ok");
    assert(result.model->fit_summary[2].status == "ok");

    bool has_monthpos_signal = false;
    for (double coeff : result.model->monthpos_block.dom_coeff) {
        has_monthpos_signal = has_monthpos_signal || std::fabs(coeff) > 0.03;
    }
    for (double coeff : result.model->monthpos_block.dme_coeff) {
        has_monthpos_signal = has_monthpos_signal || std::fabs(coeff) > 0.03;
    }
    for (double coeff : result.model->monthpos_block.lwd_coeff) {
        has_monthpos_signal = has_monthpos_signal || std::fabs(coeff) > 0.03;
    }
    assert(has_monthpos_signal);
    assert(!result.model->event_block.coeff.empty());
    assert(std::fabs(result.model->event_block.coeff[0]) > 0.03);
    assert(result.model->metadata.calendar_id == "ops");
    assert(result.model->metadata.calendar_version == "v3");

    FormalPredictContext context;
    context.task_spec = &task;
    context.event_calendar = &compiled;
    context.bucket_id = start_bucket + 90;

    FormalPrediction prediction;
    assert(PredictFormalModel(result.model.get(), context, &prediction) == error::OK);
    assert(prediction.ready);
    assert(prediction.event_enabled);
    assert(prediction.value > 0.0);
    assert(prediction.value < 1.0);

    std::printf("[PASS] Formal model trainer staged ratio fit\n");
}

void TestCandidateBuilderHoldoutTailSplit() {
    std::printf("[TEST] Candidate builder tail split...\n");

    constexpr int64_t delta = 60;
    const int64_t start_bucket = UtcBucket(2026, 1, 1, 0, 0, 0, delta);

    ValueReplaySeries replay;
    replay.key = "svc-fit";
    replay.window.has_data = true;
    replay.window.request_bucket_start = start_bucket;
    replay.window.request_bucket_end = start_bucket + 19;
    replay.window.first_bucket_id = start_bucket;
    replay.window.last_bucket_id = start_bucket + 19;
    replay.window.observation_count = 20;
    for (int i = 0; i < 20; ++i) {
        replay.points.push_back(ValueReplayPoint{start_bucket + i, 10.0 + static_cast<double>(i), 0});
    }

    const ValueFeatureProfile profile = BuildValueT1aProfile();
    ValueCandidateBuildResult result;
    assert(CandidateBuilder::BuildValue(profile, replay, 3, nullptr, delta, "UTC", nullptr, &result) ==
           CandidateBuildStatus::kTrained);
    assert(result.train_window.observation_count == 20);
    assert(result.holdout_window.observation_count == 0);
    assert(result.candidate_model != nullptr);
    assert(result.candidate_model->metadata.holdout_count == 0);

    std::printf("[PASS] Candidate builder tail split\n");
}

void TestCandidateValidatorNearZeroTolerance() {
    std::printf("[TEST] Candidate validator near-zero tolerance...\n");

    ValueFeatureProfile profile = BuildValueT1aProfile();
    profile.transform_name = "identity";

    ValueReplaySeries replay;
    replay.window.has_data = true;
    replay.window.request_bucket_start = 100;
    replay.window.request_bucket_end = 103;
    replay.window.first_bucket_id = 100;
    replay.window.last_bucket_id = 103;
    replay.window.observation_count = 4;
    replay.points.push_back(ValueReplayPoint{100, 10.0, 0});
    replay.points.push_back(ValueReplayPoint{101, 10.0, 0});
    replay.points.push_back(ValueReplayPoint{102, 10.0, 0});
    replay.points.push_back(ValueReplayPoint{103, 10.0, 0});

    ReplayWindowSummary holdout_window;
    holdout_window.has_data = true;
    holdout_window.observation_count = 2;
    holdout_window.first_bucket_id = 102;
    holdout_window.last_bucket_id = 103;

    ValueFormalModel incumbent_model;
    incumbent_model.metadata.kind = FormalModelKind::kValueBaseline;
    incumbent_model.metadata.model_version = 1;
    incumbent_model.readiness = ModelReadiness::kCoreNoMonthReady;
    incumbent_model.transform_name = "identity";
    incumbent_model.delta = 60;
    incumbent_model.tz = "UTC";
    incumbent_model.train_start = 100;
    incumbent_model.train_end = 101;
    incumbent_model.confidence_base_at_train = 1.0;
    incumbent_model.sigma_ref = 1.0;
    incumbent_model.core_block.beta0 = 10.0;

    ValueFormalModel candidate_model = incumbent_model;
    candidate_model.metadata.model_version = 2;
    candidate_model.core_block.beta0 = 10.0 + 1e-8;

    const CandidateValidationResult result = CandidateValidator::ValidateValue(
        profile,
        replay,
        holdout_window,
        &candidate_model,
        &incumbent_model,
        nullptr);
    assert(result.validation_count == 2);
    assert(result.candidate_loss > result.incumbent_loss);
    assert(result.status == CandidateValidationStatus::kPassed);
    assert(result.pass);

    std::printf("[PASS] Candidate validator near-zero tolerance\n");
}

void TestCandidateValidatorShadowPrequentialReplay() {
    std::printf("[TEST] Candidate validator shadow prequential replay...\n");

    ValueFeatureProfile profile = BuildValueT1aProfile();
    profile.transform_name = "identity";

    ValueReplaySeries replay;
    replay.window.has_data = true;
    replay.window.request_bucket_start = 100;
    replay.window.request_bucket_end = 103;
    replay.window.first_bucket_id = 100;
    replay.window.last_bucket_id = 103;
    replay.window.observation_count = 4;
    replay.points.push_back(ValueReplayPoint{100, 10.0, 0});
    replay.points.push_back(ValueReplayPoint{101, 12.0, 0});
    replay.points.push_back(ValueReplayPoint{102, 14.0, 0});
    replay.points.push_back(ValueReplayPoint{103, 16.0, 0});

    ReplayWindowSummary holdout_window;
    holdout_window.has_data = true;
    holdout_window.observation_count = 2;
    holdout_window.first_bucket_id = 102;
    holdout_window.last_bucket_id = 103;

    const auto candidate_model = BuildConstantValueFormalModel(0.0, 1.0, 2, "identity");
    const auto frozen_ref_model = BuildConstantValueFormalModel(0.0, 1.0, 1, "identity");

    ValueShadowState shadow_state;
    shadow_state.active = true;
    shadow_state.ref_kind = ShadowRefKind::kSelfFormal;
    shadow_state.ref_model_version = 1;
    shadow_state.frozen_ref_model = frozen_ref_model;

    const CandidateValidationResult result = CandidateValidator::ValidateValue(
        profile,
        replay,
        holdout_window,
        candidate_model.get(),
        nullptr,
        &shadow_state);
    assert(result.validation_count == 2);
    assert(result.status == CandidateValidationStatus::kFailed);
    assert(!result.pass);
    assert(result.incumbent_loss < result.candidate_loss);

    std::printf("[PASS] Candidate validator shadow prequential replay\n");
}

void TestRebuildOutcomeHelpers() {
    std::printf("[TEST] Rebuild outcome helpers...\n");

    struct DummyOutcome {
        RebuildCandidateState candidate_state = RebuildCandidateState::kNone;
        RebuildSwitchState switch_state = RebuildSwitchState::kIdle;
        RebuildFailureReason failure_reason = RebuildFailureReason::kNone;
        std::string failure_reason_detail;
    };

    DummyOutcome outcome;
    SetRebuildAcceptedOutcome(&outcome);
    assert(outcome.candidate_state == RebuildCandidateState::kAccepted);
    assert(outcome.switch_state == RebuildSwitchState::kFormalApplied);
    assert(outcome.failure_reason == RebuildFailureReason::kNone);
    assert(outcome.failure_reason_detail.empty());

    SetRebuildRejectedOutcome(&outcome, "holdout_failed");
    assert(outcome.candidate_state == RebuildCandidateState::kRejected);
    assert(outcome.switch_state == RebuildSwitchState::kIdle);
    assert(outcome.failure_reason == RebuildFailureReason::kValidationFailed);
    assert(outcome.failure_reason_detail == "holdout_failed");

    ApplyBuildFailureOutcome(CandidateBuildStatus::kSolverUnavailable, &outcome);
    assert(outcome.candidate_state == RebuildCandidateState::kFailed);
    assert(outcome.switch_state == RebuildSwitchState::kIdle);
    assert(outcome.failure_reason == RebuildFailureReason::kUnavailable);
    assert(outcome.failure_reason_detail == "solver_unavailable");

    ApplyValidationFailureOutcome(CandidateValidationStatus::kInsufficientHoldout, false, &outcome);
    assert(outcome.candidate_state == RebuildCandidateState::kFailed);
    assert(outcome.switch_state == RebuildSwitchState::kIdle);
    assert(outcome.failure_reason == RebuildFailureReason::kInsufficientData);
    assert(outcome.failure_reason_detail == "insufficient_holdout");

    ApplyValidationFailureOutcome(CandidateValidationStatus::kPassed,
                                  true,
                                  &outcome,
                                  RebuildSwitchState::kRebuildBlocked);
    assert(outcome.candidate_state == RebuildCandidateState::kFailed);
    assert(outcome.switch_state == RebuildSwitchState::kRebuildBlocked);
    assert(outcome.failure_reason == RebuildFailureReason::kTrainFailed);
    assert(outcome.failure_reason_detail == "full_model_train_failed");

    std::printf("[PASS] Rebuild outcome helpers\n");
}

}  // namespace

int main() {
    TestProfileConfigDefaultsAndDerivedValues();
    TestRuntimeConfigYamlLoad();
    TestRuntimeConfigYamlValidation();
    TestCalendarFeatureHelper();
    TestEventCalendarMatcherScopeAndOverlap();
    TestConfigParserTimezoneDefault();
    TestEventCalendarLocalWallClockInheritsTaskTimezone();
    TestReadinessHelper();
    TestRelationPatternFusionSupportEscape();
    TestRelationPatternFusionRemainingPatterns();
    TestCandidateBuilderRelationBasisViews();
    TestCandidateValidatorRelationAggregate();
    TestFormalModelSchemaAndPredictor();
    TestWeightedHuberRidgeBlockSolver();
    TestFormalModelTrainerStagesForValue();
    TestFormalModelTrainerStagesForRatio();
    TestCandidateBuilderHoldoutTailSplit();
    TestCandidateValidatorNearZeroTolerance();
    TestCandidateValidatorShadowPrequentialReplay();
    TestRebuildOutcomeHelpers();
    return 0;
}
