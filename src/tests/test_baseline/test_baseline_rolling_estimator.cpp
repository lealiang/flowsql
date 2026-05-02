/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <cassert>
#include <cmath>

#include <plugins/baseline/rolling/rolling_estimator.h>

using namespace flowsql;
using namespace flowsql::baseline;

namespace {

void AssertNear(double actual, double expected, double eps = 1.0e-12) {
    assert(std::fabs(actual - expected) < eps);
}

BaselineRollingConfig BuildConfig() {
    BaselineRollingConfig config;
    config.bucket_seconds = 60;
    config.timezone = "UTC";
    config.daily_harmonic_order = 0;
    config.weekly_harmonic_order = 0;
    config.q_level_scale = 0.0;
    config.q_trend_scale = 0.0;
    config.q_day_scale = 0.0;
    config.q_week_scale = 0.0;
    config.band_z = 2.0;
    config.sigma_floor = 0.05;
    config.trend_delta_max_scale = 1.0;
    config.trend_abs_max_scale = 10.0;
    return config;
}

RollingState BuildState(const BaselineRollingConfig& config) {
    RollingState state;
    assert(BuildEmptyRollingState("link-a", config, &state) == BaselineStatus::kOk);
    state.has_seen_observation = true;
    state.last_seen_bucket = 10;
    state.accepted_update_count = 10;
    state.state_status = RollingStateStatus::kWarming;
    state.theta.level = 10.0;
    state.theta.trend = 0.5;
    state.sigma = 0.1;
    state.p_level = 0.01;
    state.p_level_trend = 0.0;
    state.p_trend = 0.0025;
    return state;
}

ObservedModelPoint BuildPoint(int64_t bucket_id, double y_model) {
    ObservedModelPoint point;
    point.status = BaselineStatus::kOk;
    point.series_key = "link-a";
    point.bucket_id = bucket_id;
    point.y_model = y_model;
    point.can_score = true;
    point.can_update = true;
    point.score_weight = 1.0;
    point.update_weight = 1.0;
    return point;
}

void TestPredictAdvancesTrendAndBuildsBand() {
    const BaselineRollingConfig config = BuildConfig();
    RollingState state = BuildState(config);
    const ObservedModelPoint point = BuildPoint(12, 11.2);

    RollingEstimatorResult result;
    assert(PredictRollingState(state, point, config, &result) == BaselineStatus::kOk);
    AssertNear(result.model_mu, 11.0);
    AssertNear(result.pred_var, 0.02);
    AssertNear(result.obs_var, 0.0325);
    AssertNear(result.band_std, std::sqrt(0.0325));
    AssertNear(result.model_lower, 11.0 - 2.0 * std::sqrt(0.0325));
    AssertNear(result.model_upper, 11.0 + 2.0 * std::sqrt(0.0325));
    AssertNear(result.residual, 0.2);
    AssertNear(result.z_score, 0.2 / std::sqrt(0.0325));
}

void TestUpdateMovesLevelAndStoresBucket() {
    const BaselineRollingConfig config = BuildConfig();
    RollingState state = BuildState(config);
    const ObservedModelPoint point = BuildPoint(12, 12.0);

    RollingEstimatorResult result;
    assert(UpdateRollingStateWithObservation(point, config, &state, &result) ==
           BaselineStatus::kOk);
    assert(state.last_seen_bucket == 12);
    assert(state.accepted_update_count == 11);
    assert(state.theta.level > 11.0);
    assert(state.theta.trend > 0.5);
    assert(state.p_level <= result.pred_p_level);
    AssertNear(result.residual, 1.0);
}

void TestRejectsDuplicateAndOutOfOrderBucketWithoutMutation() {
    const BaselineRollingConfig config = BuildConfig();
    RollingState state = BuildState(config);
    const RollingState before = state;

    RollingEstimatorResult result;
    assert(UpdateRollingStateWithObservation(BuildPoint(10, 10.0), config, &state, &result) ==
           BaselineStatus::kInvalidArgument);
    assert(state.last_seen_bucket == before.last_seen_bucket);
    AssertNear(state.theta.level, before.theta.level);

    assert(UpdateRollingStateWithObservation(BuildPoint(9, 10.0), config, &state, &result) ==
           BaselineStatus::kInvalidArgument);
    assert(state.last_seen_bucket == before.last_seen_bucket);
    AssertNear(state.theta.level, before.theta.level);
}

void TestHarmonicPredictionUsesLocalPhase() {
    BaselineRollingConfig config = BuildConfig();
    config.bucket_seconds = 3600;
    config.daily_harmonic_order = 1;
    RollingState state;
    assert(BuildEmptyRollingState("link-a", config, &state) == BaselineStatus::kOk);
    state.has_seen_observation = true;
    state.last_seen_bucket = 5;
    state.theta.level = 10.0;
    state.theta.daily.sin_coeff[0] = 2.0;
    state.theta.daily.cos_coeff[0] = 3.0;
    state.sigma = 0.1;

    RollingEstimatorResult result;
    assert(PredictRollingState(state, BuildPoint(6, 12.0), config, &result) ==
           BaselineStatus::kOk);
    AssertNear(result.model_mu, 12.0, 1.0e-9);
}

void TestForecastViewCapsLocalTrendAndUsesSnapshotVariance() {
    BaselineRollingConfig config = BuildConfig();
    config.forecast_trend_cap_buckets = 3;
    config.q_level_scale = 100.0;
    config.q_trend_scale = 100.0;
    RollingState state = BuildState(config);

    RollingEstimatorResult one_step;
    assert(PredictRollingForecastState(state, 11, config, &one_step) ==
           BaselineStatus::kOk);
    AssertNear(one_step.model_mu, 10.5);
    AssertNear(one_step.pred_var, 0.01);

    RollingEstimatorResult long_horizon;
    assert(PredictRollingForecastState(state, 20, config, &long_horizon) ==
           BaselineStatus::kOk);
    AssertNear(long_horizon.model_mu, 11.5);
    AssertNear(long_horizon.pred_var, 0.01);
    AssertNear(long_horizon.obs_var, 0.0225);
    AssertNear(long_horizon.band_std, std::sqrt(0.0225));
}

void TestCannotUpdateLowSupportPoint() {
    const BaselineRollingConfig config = BuildConfig();
    RollingState state = BuildState(config);
    ObservedModelPoint point = BuildPoint(12, 12.0);
    point.can_update = false;
    point.update_weight = 0.0;

    RollingEstimatorResult result;
    assert(UpdateRollingStateWithObservation(point, config, &state, &result) ==
           BaselineStatus::kOk);
    assert(state.last_seen_bucket == 10);
    AssertNear(state.theta.level, 10.0);
    assert(!result.did_update);
}

void TestAdaptBoostSpeedsLevelAndKeepsSeasonalityConservative() {
    BaselineRollingConfig config = BuildConfig();
    config.daily_harmonic_order = 1;
    config.q_level_scale = 0.1;
    config.max_q_boost = 9.0;
    config.max_level_boost = 4.0;
    config.day_learning_scale = 1.0;
    config.seasonal_drift_min_scale = 0.1;
    config.day_delta_coeff_max_scale = 100.0;

    RollingState base_state;
    assert(BuildEmptyRollingState("link-a", config, &base_state) == BaselineStatus::kOk);
    base_state.has_seen_observation = true;
    base_state.last_seen_bucket = 5;
    base_state.accepted_update_count = 10;
    base_state.state_status = RollingStateStatus::kWarming;
    base_state.theta.level = 10.0;
    base_state.theta.trend = 0.0;
    base_state.theta.daily.sin_coeff[0] = 0.0;
    base_state.theta.daily.cos_coeff[0] = 0.0;
    base_state.sigma = 0.2;
    base_state.p_level = 0.01;
    base_state.p_trend = 0.0;
    base_state.theta.daily.sin_p[0] = 0.01;
    base_state.theta.daily.cos_p[0] = 0.01;

    RollingState normal = base_state;
    RollingState boosted = base_state;
    const ObservedModelPoint point = BuildPoint(6, 13.0);
    RollingEstimatorResult normal_result;
    RollingEstimatorResult boosted_result;
    assert(UpdateRollingStateWithObservation(point, config, &normal, &normal_result, 0.0) ==
           BaselineStatus::kOk);
    assert(UpdateRollingStateWithObservation(point, config, &boosted, &boosted_result, 1.0) ==
           BaselineStatus::kOk);

    assert(boosted.theta.level - base_state.theta.level >
           normal.theta.level - base_state.theta.level);
    assert(boosted_result.pred_p_level > normal_result.pred_p_level);

    const double normal_day_abs = std::fabs(normal.theta.daily.sin_coeff[0]) +
                                  std::fabs(normal.theta.daily.cos_coeff[0]);
    const double boosted_day_abs = std::fabs(boosted.theta.daily.sin_coeff[0]) +
                                   std::fabs(boosted.theta.daily.cos_coeff[0]);
    assert(boosted_day_abs < normal_day_abs);
}

BaselineTaskSpec BuildSeededValueSpec() {
    BaselineTaskSpec spec;
    spec.task_id = "rolling-seeded-estimator";
    spec.task_kind = "value";
    spec.feature_type = "value_basic";
    spec.feature_id = "bps";
    spec.profile = "default";
    spec.clock_spec.bucket_seconds = 60;
    spec.clock_spec.timezone = "UTC";
    return spec;
}

BootstrapSeed BuildFullDailySeed() {
    BootstrapSeed seed;
    seed.artifact_kind = BootstrapArtifactKind::kValue;
    seed.seed_status = BootstrapSeedStatus::kFull;
    seed.series_key = "link-a";
    seed.task_identity.task_id = "rolling-seeded-estimator";
    seed.task_identity.task_kind = "value";
    seed.task_identity.feature_type = "value_basic";
    seed.task_identity.feature_id = "bps";
    seed.task_identity.profile = "default";
    seed.clock_spec.bucket_seconds = 60;
    seed.clock_spec.timezone = "UTC";
    seed.coverage_report.accepted_count = 20160;
    seed.coverage_report.train_start_bucket = 0;
    seed.coverage_report.train_end_bucket = 20;
    seed.coverage_report.coverage_ratio = 1.0;
    seed.seeded_components = {"level", "trend", "daily"};
    seed.enabled_components = {"level", "trend", "daily"};
    seed.theta_init.available = true;
    seed.theta_init.model_space = "log1p";
    seed.theta_init.reference_bucket_id = 20;
    seed.theta_init.level = 10.0;
    seed.theta_init.trend = 0.0;
    seed.theta_init.daily_harmonic.push_back(BootstrapHarmonicInit{1, 1.0, 0.5});
    seed.sigma_init.available = true;
    seed.sigma_init.model_space = "log1p";
    seed.sigma_init.value = 0.2;
    seed.uncertainty_init.available = true;
    seed.uncertainty_init.coverage_ratio = 1.0;
    seed.uncertainty_init.component_uncertainty.level_scale = 1.0;
    seed.uncertainty_init.component_uncertainty.trend_scale = 1.0;
    seed.uncertainty_init.component_uncertainty.daily_scale = 1.0;
    seed.uncertainty_init.component_uncertainty.weekly_scale = 1.0;
    seed.maturity_init.available = true;
    seed.maturity_init.seed_status = BootstrapSeedStatus::kFull;
    seed.maturity_init.confidence = 0.8;
    seed.maturity_init.accepted_count = 20160;
    seed.maturity_init.coverage_ratio = 1.0;
    return seed;
}

void TestFullBootstrapSeedFreezesSeasonalityDuringDriftLearning() {
    BaselineRollingConfig config = BuildConfig();
    config.daily_harmonic_order = 1;
    config.day_learning_scale = 1.0;
    config.day_delta_coeff_max_scale = 100.0;
    config.max_level_boost = 4.0;
    config.seasonal_drift_min_scale = 0.1;

    RollingState state;
    std::string diagnostics;
    assert(InitializeRollingStateFromBootstrapSeed(BuildSeededValueSpec(),
                                                   "link-a",
                                                   BuildFullDailySeed(),
                                                   config,
                                                   &state,
                                                   &diagnostics) == BaselineStatus::kOk);
    state.p_level = 0.01;
    state.theta.daily.sin_p[0] = 0.01;
    state.theta.daily.cos_p[0] = 0.01;

    const double level_before = state.theta.level;
    const double day_sin_before = state.theta.daily.sin_coeff[0];
    const double day_cos_before = state.theta.daily.cos_coeff[0];

    RollingEstimatorResult result;
    assert(UpdateRollingStateWithObservation(BuildPoint(21, 13.0),
                                             config,
                                             &state,
                                             &result,
                                             1.0) == BaselineStatus::kOk);

    assert(state.theta.level > level_before);
    AssertNear(state.theta.daily.sin_coeff[0], day_sin_before);
    AssertNear(state.theta.daily.cos_coeff[0], day_cos_before);
}

}  // namespace

int main() {
    TestPredictAdvancesTrendAndBuildsBand();
    TestUpdateMovesLevelAndStoresBucket();
    TestRejectsDuplicateAndOutOfOrderBucketWithoutMutation();
    TestHarmonicPredictionUsesLocalPhase();
    TestForecastViewCapsLocalTrendAndUsesSnapshotVariance();
    TestCannotUpdateLowSupportPoint();
    TestAdaptBoostSpeedsLevelAndKeepsSeasonalityConservative();
    TestFullBootstrapSeedFreezesSeasonalityDuringDriftLearning();
    return 0;
}
