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
    AssertNear(config.band_z, 3.0);
    AssertNear(config.sigma_floor, 0.05);
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
  rolling_config:
    n_min_score: 5
    n_min_update: 20
    n_ref: 20
    d_min_score: 12
    d_min_update: 120
    d_ref: 120
    daily_harmonic_order: 4
    weekly_harmonic_order: 2
    band_z: 2.5
    sigma_floor: 0.07
    process_noise_gap_cap_buckets: 600
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
    AssertNear(config.sigma_floor, 0.07);

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
    AssertNear(config.band_z, 3.0);
    AssertNear(config.sigma_floor, 0.05);

    ResetBaselineRuntimeConfig();
}

}  // namespace

int main() {
    TestRollingConfigDefaultsAndDerivedBuckets();
    TestRollingConfigRuntimeOverride();
    TestRollingConfigRejectsInvalidThresholds();
    TestRollingConfigTemplateCanBeLoadedStrictly();
    return 0;
}
