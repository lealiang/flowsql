/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <cassert>
#include <cmath>
#include <fstream>
#include <string>

#include <common/error_code.h>
#include <plugins/baseline/bootstrap/bootstrap_types.h>
#include <plugins/baseline/config/runtime_config.h>
#include <plugins/baseline/model/task_spec.h>
#include <plugins/baseline/rolling/rolling_config.h>

using namespace flowsql;
using namespace flowsql::baseline;

namespace {

BaselineTaskSpec BuildTaskSpec(int64_t bucket_seconds) {
    BaselineTaskSpec spec;
    spec.task_id = "rolling-config-test";
    spec.task_kind = "value";
    spec.feature_type = "value_basic";
    spec.feature_id = "bps";
    spec.profile = "default";
    spec.clock_spec.bucket_seconds = bucket_seconds;
    spec.clock_spec.timezone = "Asia/Shanghai";
    spec.delta = bucket_seconds;
    spec.tz = "Asia/Shanghai";
    return spec;
}

void AssertNear(double actual, double expected) {
    assert(std::fabs(actual - expected) < 1.0e-12);
}

void TestRollingConfigDefaultsAndDerivedBuckets() {
    ResetBaselineRuntimeConfig();
    BaselineRollingConfig config;
    std::string err;
    const BaselineStatus status =
        ResolveBaselineRollingConfig(BuildTaskSpec(300), &config, &err);
    assert(status == BaselineStatus::kOk);
    assert(config.bucket_seconds == 300);
    assert(config.day_buckets == 288);
    assert(config.week_buckets == 2016);
    assert(config.process_noise_gap_cap_buckets == 2016);
    assert(config.min_ready_hint_updates == 288);
    assert(config.daily_harmonic_order == 6);
    assert(config.weekly_harmonic_order == 3);
    assert(config.n_min_score == 3);
    assert(config.n_min_update == 10);
    assert(config.n_ref == 10);
    AssertNear(config.band_z, 1.96);
    AssertNear(config.forecast_band_z, 3.0);
    AssertNear(config.sigma_floor, 0.05);
    AssertNear(config.full_seed_seasonal_scale, 0.1);
    AssertNear(config.partial_seed_seasonal_scale, 0.5);
    assert(config.freeze_seeded_seasonal_on_drift);
    AssertNear(config.level_shift_reference_z, 0.5);
    AssertNear(config.level_shift_cusum_decay, 0.98);
    AssertNear(config.level_shift_cusum_threshold, 16.0);
    assert(config.level_ready_min_updates == 30);
    assert(config.min_warming_updates == 10);
    assert(config.score_warming_min_updates == 60);
    assert(config.score_ready_min_updates == 240);
    assert(config.calibration_warmup_min_updates == 60);
    assert(config.daily_coverage_bins == 24);
    assert(config.weekly_coverage_bins == 168);
    assert(config.daily_ready_min_days == 2);
    assert(config.weekly_ready_min_weeks == 2);
    AssertNear(config.calibration_coverage_floor, 0.98);
    AssertNear(config.missing_daily_uncertainty_scale, 9.0);
    AssertNear(config.level_only_extreme_z, 8.0);
    AssertNear(config.detection_band_std_cap, 0.5);
    assert(config.forecast_trend_cap_buckets == config.day_buckets);
    AssertNear(config.monthpos_alpha, 0.005);
    assert(config.monthpos_min_month_transitions == 2);
    assert(config.relation_rolling.enable_routed_rolling);
    assert(config.relation_rolling.enable_stream_basis);
    assert(config.relation_rolling.include_universal_summaries_without_basis);
    assert(config.relation_rolling.basis_stats_max_groups == 256);
    assert(config.relation_rolling.basis_collect_min_buckets == 0);
    assert(config.relation_rolling.basis_replacement_cap_max == 2);
    assert(config.relation_rolling.routed_state_shard_count == 16);
    assert(config.relation_rolling.relation_fusion.enable_relation_fusion);
    AssertNear(config.relation_rolling.relation_fusion.fusion_z_score_cap, 5.0);
    AssertNear(config.relation_rolling.relation_fusion.fusion_min_evidence_score, 0.20);
    assert(config.relation_rolling.relation_fusion.fusion_persistence_window == 2);
    AssertNear(config.relation_rolling.relation_fusion.fusion_warming_weight, 0.25);
    AssertNear(config.relation_rolling.relation_fusion.fusion_degraded_weight, 0.25);
    assert(config.relation_rolling.relation_fusion.dominant_single_cap == 3);
    assert(config.relation_rolling.relation_fusion.dominant_pattern_cap == 2);
    assert(config.relation_rolling.relation_fusion.fusion_state_ttl_buckets == 20160);
    assert(config.relation_rolling.relation_fusion.fusion_state_max_sources == 4096);
    assert(config.relation_rolling.relation_fusion.fusion_state_cleanup_interval_updates == 512);
    assert(config.relation_rolling.relation_fusion.fusion_state_cleanup_scan_limit == 256);
    assert(config.relation_rolling.relation_fusion.fusion_persistence_max_keys_per_source == 512);

    BootstrapSeedQualityConfig seed_quality;
    assert(TryGetBootstrapSeedQualityConfigOverride(&seed_quality));
    AssertNear(seed_quality.full_min_coverage_ratio, 0.90);
    AssertNear(seed_quality.partial_min_coverage_ratio, 0.50);
    assert(seed_quality.daily_min_span_days == 1);
    assert(seed_quality.weekly_min_span_days == 14);
}

void TestRollingConfigRuntimeOverride() {
    const std::string config_path = "/tmp/flowsql_baseline_rolling_config_test.yaml";
    {
        std::ofstream file(config_path);
        file << R"(
baseline:
  parser:
    tz_default: "Asia/Shanghai"
  shared_profile_config:
    daily_harmonic_order: 4
    weekly_harmonic_order: 2
    dme_max: 7
    m_month_enable: 4
    month_cov_min: 0.8
    lambda_season: 1.0
    lambda_dom: 4.0
    lambda_dme: 2.0
    lambda_lwd: 1.0
    lambda_event: 2.0
  bootstrap:
    seed_quality:
      full_min_coverage_ratio: 0.85
      partial_min_coverage_ratio: 0.40
      daily_min_span_days: 1
      weekly_min_span_days: 10
      daily_phase_coverage_ratio: 0.65
      weekly_phase_coverage_ratio: 0.55
  rolling_config:
    n_min_score: 5
    n_min_update: 20
    n_ref: 20
    d_min_score: 12
    d_min_update: 120
    d_ref: 120
    band_z: 2.5
    forecast_band_z: 3.5
    sigma_floor: 0.07
    full_seed_seasonal_scale: 0.2
    partial_seed_seasonal_scale: 0.6
    freeze_seeded_seasonal_on_drift: false
    process_noise_gap_cap_buckets: 600
    level_ready_min_updates: 7
    score_warming_min_updates: 8
    score_ready_min_updates: 12
    score_recovery_min_updates: 9
    score_drift_degrade_start: 1.25
    level_shift_reference_z: 0.4
    level_shift_cusum_decay: 0.90
    level_shift_cusum_threshold: 5.0
    calibration_alpha: 0.03
    calibration_warmup_min_updates: 8
    calibration_coverage_floor: 0.95
    calibration_tail3_limit: 0.03
    calibration_tail5_limit: 0.004
    calibration_multiplier_min: 1.1
    calibration_multiplier_max: 5.5
    daily_coverage_bins: 12
    weekly_coverage_bins: 84
    daily_ready_min_days: 1
    daily_ready_coverage_ratio: 0.6
    weekly_ready_min_weeks: 1
    weekly_ready_coverage_ratio: 0.5
    maturity_uncertainty_cold_scale: 8.0
    maturity_uncertainty_warming_scale: 3.0
    maturity_uncertainty_drift_scale: 3.5
    maturity_uncertainty_recalibrating_scale: 1.5
    missing_daily_uncertainty_scale: 7.0
    missing_weekly_uncertainty_scale: 3.0
    level_only_extreme_z: 7.5
    detection_band_std_cap: 1.2
    forecast_trend_cap_buckets: 720
    monthpos_alpha: 0.004
    monthpos_delta_max_scale: 0.4
    monthpos_min_month_transitions: 1
    monthpos_ready_coverage_ratio: 0.5
    relation_rolling:
      enable_routed_rolling: true
      enable_stream_basis: true
      include_universal_summaries_without_basis: true
      basis_stats_max_groups: 512
      basis_collect_min_buckets: 10
      basis_ready_min_buckets: 30
      basis_refresh_interval_buckets: 20
      basis_candidate_min_coverage_ratio: 0.70
      basis_replacement_cap_ratio: 0.30
      basis_replacement_cap_max: 3
      basis_handover_warmup_buckets: 40
      basis_threshold_margin: 1.30
      basis_min_stable_refresh_count: 3
      routed_state_shard_count: 32
      relation_fusion:
        enable_relation_fusion: false
        fusion_z_score_cap: 4.0
        fusion_min_evidence_score: 0.30
        fusion_persistence_window: 3
        fusion_warming_weight: 0.20
        fusion_degraded_weight: 0.10
        fusion_support_weight: 0.40
        fusion_oppose_weight: 0.60
        basic_pattern_weight: 0.65
        stable_head_pattern_weight: 0.80
        dominant_single_cap: 4
        dominant_pattern_cap: 3
        fusion_state_ttl_buckets: 64
        fusion_state_max_sources: 8
        fusion_state_cleanup_interval_updates: 2
        fusion_state_cleanup_scan_limit: 4
        fusion_persistence_max_keys_per_source: 32
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

    std::string err;
    assert(LoadBaselineRuntimeConfigFromYaml(config_path, true, &err) == error::OK);
    BaselineRollingConfig config;
    const BaselineStatus status =
        ResolveBaselineRollingConfig(BuildTaskSpec(60), &config, &err);
    assert(status == BaselineStatus::kOk);
    assert(config.day_buckets == 1440);
    assert(config.week_buckets == 10080);
    assert(config.process_noise_gap_cap_buckets == 600);
    assert(config.min_ready_hint_updates == 1440);
    assert(config.n_min_score == 5);
    assert(config.n_min_update == 20);
    assert(config.n_ref == 20);
    assert(config.d_min_score == 12);
    assert(config.d_min_update == 120);
    assert(config.d_ref == 120);
    assert(config.daily_harmonic_order == 4);
    assert(config.weekly_harmonic_order == 2);
    AssertNear(config.band_z, 2.5);
    AssertNear(config.forecast_band_z, 3.5);
    AssertNear(config.sigma_floor, 0.07);
    AssertNear(config.full_seed_seasonal_scale, 0.2);
    AssertNear(config.partial_seed_seasonal_scale, 0.6);
    assert(!config.freeze_seeded_seasonal_on_drift);
    assert(config.level_ready_min_updates == 7);
    assert(config.score_warming_min_updates == 8);
    assert(config.score_ready_min_updates == 12);
    assert(config.score_recovery_min_updates == 9);
    AssertNear(config.score_drift_degrade_start, 1.25);
    AssertNear(config.level_shift_reference_z, 0.4);
    AssertNear(config.level_shift_cusum_decay, 0.90);
    AssertNear(config.level_shift_cusum_threshold, 5.0);
    AssertNear(config.calibration_alpha, 0.03);
    assert(config.calibration_warmup_min_updates == 8);
    AssertNear(config.calibration_coverage_floor, 0.95);
    AssertNear(config.calibration_tail3_limit, 0.03);
    AssertNear(config.calibration_tail5_limit, 0.004);
    AssertNear(config.calibration_multiplier_min, 1.1);
    AssertNear(config.calibration_multiplier_max, 5.5);
    assert(config.daily_coverage_bins == 12);
    assert(config.weekly_coverage_bins == 84);
    assert(config.daily_ready_min_days == 1);
    AssertNear(config.daily_ready_coverage_ratio, 0.6);
    assert(config.weekly_ready_min_weeks == 1);
    AssertNear(config.weekly_ready_coverage_ratio, 0.5);
    AssertNear(config.maturity_uncertainty_cold_scale, 8.0);
    AssertNear(config.maturity_uncertainty_warming_scale, 3.0);
    AssertNear(config.maturity_uncertainty_drift_scale, 3.5);
    AssertNear(config.maturity_uncertainty_recalibrating_scale, 1.5);
    AssertNear(config.missing_daily_uncertainty_scale, 7.0);
    AssertNear(config.missing_weekly_uncertainty_scale, 3.0);
    AssertNear(config.level_only_extreme_z, 7.5);
    AssertNear(config.detection_band_std_cap, 1.2);
    assert(config.forecast_trend_cap_buckets == 720);
    AssertNear(config.monthpos_alpha, 0.004);
    AssertNear(config.monthpos_delta_max_scale, 0.4);
    assert(config.monthpos_min_month_transitions == 1);
    AssertNear(config.monthpos_ready_coverage_ratio, 0.5);
    assert(config.relation_rolling.basis_stats_max_groups == 512);
    assert(config.relation_rolling.basis_collect_min_buckets == 10);
    assert(config.relation_rolling.basis_ready_min_buckets == 30);
    assert(config.relation_rolling.basis_refresh_interval_buckets == 20);
    AssertNear(config.relation_rolling.basis_candidate_min_coverage_ratio, 0.70);
    AssertNear(config.relation_rolling.basis_replacement_cap_ratio, 0.30);
    assert(config.relation_rolling.basis_replacement_cap_max == 3);
    assert(config.relation_rolling.basis_handover_warmup_buckets == 40);
    AssertNear(config.relation_rolling.basis_threshold_margin, 1.30);
    assert(config.relation_rolling.basis_min_stable_refresh_count == 3);
    assert(config.relation_rolling.routed_state_shard_count == 32);
    assert(!config.relation_rolling.relation_fusion.enable_relation_fusion);
    AssertNear(config.relation_rolling.relation_fusion.fusion_z_score_cap, 4.0);
    AssertNear(config.relation_rolling.relation_fusion.fusion_min_evidence_score, 0.30);
    assert(config.relation_rolling.relation_fusion.fusion_persistence_window == 3);
    AssertNear(config.relation_rolling.relation_fusion.fusion_warming_weight, 0.20);
    AssertNear(config.relation_rolling.relation_fusion.fusion_degraded_weight, 0.10);
    AssertNear(config.relation_rolling.relation_fusion.fusion_support_weight, 0.40);
    AssertNear(config.relation_rolling.relation_fusion.fusion_oppose_weight, 0.60);
    AssertNear(config.relation_rolling.relation_fusion.basic_pattern_weight, 0.65);
    AssertNear(config.relation_rolling.relation_fusion.stable_head_pattern_weight, 0.80);
    assert(config.relation_rolling.relation_fusion.dominant_single_cap == 4);
    assert(config.relation_rolling.relation_fusion.dominant_pattern_cap == 3);
    assert(config.relation_rolling.relation_fusion.fusion_state_ttl_buckets == 64);
    assert(config.relation_rolling.relation_fusion.fusion_state_max_sources == 8);
    assert(config.relation_rolling.relation_fusion.fusion_state_cleanup_interval_updates == 2);
    assert(config.relation_rolling.relation_fusion.fusion_state_cleanup_scan_limit == 4);
    assert(config.relation_rolling.relation_fusion.fusion_persistence_max_keys_per_source == 32);
    BootstrapSeedQualityConfig seed_quality;
    assert(TryGetBootstrapSeedQualityConfigOverride(&seed_quality));
    AssertNear(seed_quality.full_min_coverage_ratio, 0.85);
    AssertNear(seed_quality.partial_min_coverage_ratio, 0.40);
    assert(seed_quality.weekly_min_span_days == 10);
    AssertNear(seed_quality.weekly_phase_coverage_ratio, 0.55);

    ResetBaselineRuntimeConfig();
}

void TestRollingConfigRejectsLegacyHarmonicOverride() {
    const std::string rolling_config_path =
        "/tmp/flowsql_baseline_rolling_config_legacy_rolling_harmonic.yaml";
    {
        std::ofstream file(rolling_config_path);
        file << R"(
baseline:
  parser:
    tz_default: "Asia/Shanghai"
  shared_profile_config:
    daily_harmonic_order: 6
    weekly_harmonic_order: 3
  rolling_config:
    daily_harmonic_order: 4
    weekly_harmonic_order: 2
)";
        assert(file.good());
    }

    ResetBaselineRuntimeConfig();
    std::string err;
    assert(LoadBaselineRuntimeConfigFromYaml(rolling_config_path, true, &err) ==
           error::BAD_REQUEST);
    assert(err.find("rolling_config.daily_harmonic_order") != std::string::npos ||
           err.find("rolling_config.weekly_harmonic_order") != std::string::npos);
    ResetBaselineRuntimeConfig();

    err.clear();
    assert(LoadBaselineRuntimeConfigFromYaml(rolling_config_path, false, &err) ==
           error::BAD_REQUEST);
    assert(err.find("rolling_config.daily_harmonic_order") != std::string::npos ||
           err.find("rolling_config.weekly_harmonic_order") != std::string::npos);
    ResetBaselineRuntimeConfig();

    const std::string shared_config_path =
        "/tmp/flowsql_baseline_rolling_config_legacy_shared_harmonic.yaml";
    {
        std::ofstream file(shared_config_path);
        file << R"(
baseline:
  shared_profile_config:
    k_day: 6
    k_week: 3
)";
        assert(file.good());
    }

    err.clear();
    assert(LoadBaselineRuntimeConfigFromYaml(shared_config_path, true, &err) ==
           error::BAD_REQUEST);
    assert(err.find("shared_profile_config.k_day") != std::string::npos ||
           err.find("shared_profile_config.k_week") != std::string::npos);
    ResetBaselineRuntimeConfig();

    err.clear();
    assert(LoadBaselineRuntimeConfigFromYaml(shared_config_path, false, &err) ==
           error::BAD_REQUEST);
    assert(err.find("shared_profile_config.k_day") != std::string::npos ||
           err.find("shared_profile_config.k_week") != std::string::npos);
    ResetBaselineRuntimeConfig();
}

void TestRollingConfigRejectsInvalidThresholds() {
    const std::string config_path = "/tmp/flowsql_baseline_rolling_config_invalid.yaml";
    {
        std::ofstream file(config_path);
        file << R"(
baseline:
  parser:
    tz_default: "Asia/Shanghai"
  rolling_config:
    z_downweight: 4.0
    z_skip: 3.0
  shared_profile_config:
    daily_harmonic_order: 6
    weekly_harmonic_order: 3
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

    std::string err;
    assert(LoadBaselineRuntimeConfigFromYaml(config_path, true, &err) == error::BAD_REQUEST);
    assert(err.find("z_skip") != std::string::npos);
    ResetBaselineRuntimeConfig();

    const std::string invalid_value_ref_path =
        "/tmp/flowsql_baseline_rolling_config_invalid_value_ref.yaml";
    {
        std::ofstream file(invalid_value_ref_path);
        file << R"(
baseline:
  rolling_config:
    n_min_score: 10
    n_ref: 5
)";
        assert(file.good());
    }

    err.clear();
    assert(LoadBaselineRuntimeConfigFromYaml(invalid_value_ref_path, false, &err) ==
           error::BAD_REQUEST);
    assert(err.find("sampled value thresholds") != std::string::npos);
    ResetBaselineRuntimeConfig();

    const std::string invalid_ratio_ref_path =
        "/tmp/flowsql_baseline_rolling_config_invalid_ratio_ref.yaml";
    {
        std::ofstream file(invalid_ratio_ref_path);
        file << R"(
baseline:
  rolling_config:
    d_min_score: 100
    d_ref: 50
)";
        assert(file.good());
    }

    err.clear();
    assert(LoadBaselineRuntimeConfigFromYaml(invalid_ratio_ref_path, false, &err) ==
           error::BAD_REQUEST);
    assert(err.find("ratio denominator thresholds") != std::string::npos);
    ResetBaselineRuntimeConfig();
}

void TestRollingConfigRejectsInvalidRelationFusionCleanup() {
    const std::string config_path =
        "/tmp/flowsql_baseline_rolling_config_invalid_fusion_cleanup.yaml";
    {
        std::ofstream file(config_path);
        file << R"(
baseline:
  rolling_config:
    relation_rolling:
      relation_fusion:
        fusion_state_max_sources: 0
)";
        assert(file.good());
    }

    std::string err;
    assert(LoadBaselineRuntimeConfigFromYaml(config_path, false, &err) ==
           error::BAD_REQUEST);
    assert(err.find("relation_fusion") != std::string::npos);
    ResetBaselineRuntimeConfig();
}

void TestRollingConfigTemplateCanBeLoadedStrictly() {
    const std::string config_path =
        std::string(FLOWSQL_SOURCE_DIR) +
        "/plugins/baseline/config/baseline-config-template.yaml";

    ResetBaselineRuntimeConfig();
    std::string err;
    assert(LoadBaselineRuntimeConfigFromYaml(config_path, true, &err) == error::OK);

    BaselineRollingConfig config;
    const BaselineStatus status =
        ResolveBaselineRollingConfig(BuildTaskSpec(60), &config, &err);
    assert(status == BaselineStatus::kOk);
    assert(config.daily_harmonic_order == 6);
    assert(config.weekly_harmonic_order == 3);
    AssertNear(config.level_learning_scale, 1.0);
    AssertNear(config.day_learning_scale, 0.2);
    AssertNear(config.week_learning_scale, 0.05);
    AssertNear(config.full_seed_seasonal_scale, 0.1);
    AssertNear(config.partial_seed_seasonal_scale, 0.5);
    assert(config.freeze_seeded_seasonal_on_drift);
    AssertNear(config.level_shift_reference_z, 0.5);
    AssertNear(config.level_shift_cusum_decay, 0.98);
    AssertNear(config.level_shift_cusum_threshold, 16.0);
    AssertNear(config.band_z, 1.96);
    AssertNear(config.sigma_floor, 0.05);
    assert(config.level_ready_min_updates == 30);
    assert(config.score_warming_min_updates == 60);
    assert(config.score_ready_min_updates == 240);
    assert(config.daily_coverage_bins == 24);
    assert(config.weekly_coverage_bins == 168);
    AssertNear(config.calibration_coverage_floor, 0.98);
    AssertNear(config.calibration_multiplier_max, 6.0);
    AssertNear(config.detection_band_std_cap, 0.5);
    AssertNear(config.monthpos_ready_coverage_ratio, 0.60);
    assert(config.relation_rolling.relation_fusion.fusion_state_ttl_buckets == 20160);
    assert(config.relation_rolling.relation_fusion.fusion_state_max_sources == 4096);
    assert(config.relation_rolling.relation_fusion.fusion_state_cleanup_interval_updates == 512);
    assert(config.relation_rolling.relation_fusion.fusion_state_cleanup_scan_limit == 256);
    assert(config.relation_rolling.relation_fusion.fusion_persistence_max_keys_per_source == 512);
    BootstrapSeedQualityConfig seed_quality;
    assert(TryGetBootstrapSeedQualityConfigOverride(&seed_quality));
    AssertNear(seed_quality.full_min_coverage_ratio, 0.90);
    AssertNear(seed_quality.partial_min_coverage_ratio, 0.50);
    assert(seed_quality.weekly_min_span_days == 14);

    ResetBaselineRuntimeConfig();
}

}  // namespace

int main() {
    TestRollingConfigDefaultsAndDerivedBuckets();
    TestRollingConfigRuntimeOverride();
    TestRollingConfigRejectsLegacyHarmonicOverride();
    TestRollingConfigRejectsInvalidThresholds();
    TestRollingConfigRejectsInvalidRelationFusionCleanup();
    TestRollingConfigTemplateCanBeLoadedStrictly();
    return 0;
}
