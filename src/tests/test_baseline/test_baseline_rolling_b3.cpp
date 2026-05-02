/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <cassert>
#include <cmath>
#include <string>

#include <plugins/baseline/rolling/detection_calibration.h>
#include <plugins/baseline/rolling/maturity_gate.h>
#include <plugins/baseline/rolling/monthpos_state.h>
#include <plugins/baseline/rolling/rolling_estimator.h>
#include <plugins/baseline/rolling/rolling_state.h>
#include <plugins/baseline/rolling/score_trust.h>

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
    config.daily_harmonic_order = 1;
    config.weekly_harmonic_order = 1;
    config.level_ready_min_updates = 3;
    config.score_warming_min_updates = 2;
    config.score_ready_min_updates = 4;
    config.score_recovery_min_updates = 2;
    config.calibration_warmup_min_updates = 2;
    config.daily_ready_min_days = 1;
    config.weekly_ready_min_weeks = 1;
    config.daily_ready_coverage_ratio = 0.25;
    config.weekly_ready_coverage_ratio = 0.10;
    config.daily_coverage_bins = 4;
    config.weekly_coverage_bins = 8;
    config.sigma_floor = 0.5;
    return config;
}

BaselineTaskSpec BuildSpec() {
    BaselineTaskSpec spec;
    spec.task_id = "b3-test-task";
    spec.task_kind = "value";
    spec.feature_type = "value_basic";
    spec.feature_id = "bps";
    spec.profile = "default";
    spec.clock_spec.bucket_seconds = 60;
    spec.clock_spec.timezone = "UTC";
    spec.delta = 60;
    spec.tz = "UTC";
    return spec;
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

BootstrapSeed BuildSeed() {
    BootstrapSeed seed;
    seed.artifact_kind = BootstrapArtifactKind::kValue;
    seed.seed_status = BootstrapSeedStatus::kFull;
    seed.series_key = "link-a";
    seed.task_identity.task_id = "b3-test-task";
    seed.task_identity.task_kind = "value";
    seed.task_identity.feature_type = "value_basic";
    seed.task_identity.feature_id = "bps";
    seed.task_identity.profile = "default";
    seed.clock_spec.bucket_seconds = 60;
    seed.clock_spec.timezone = "UTC";
    seed.coverage_report.accepted_count = 20000;
    seed.coverage_report.train_start_bucket = 0;
    seed.coverage_report.train_end_bucket = 20000;
    seed.coverage_report.coverage_ratio = 0.9;
    seed.enabled_components = {"level", "daily", "weekly"};
    seed.theta_init.available = true;
    seed.theta_init.model_space = "log1p";
    seed.theta_init.reference_bucket_id = 20000;
    seed.theta_init.level = 10.0;
    seed.theta_init.daily_harmonic.push_back(BootstrapHarmonicInit{1, 0.1, 0.2});
    seed.theta_init.weekly_harmonic.push_back(BootstrapHarmonicInit{1, 0.3, 0.4});
    seed.sigma_init.available = true;
    seed.sigma_init.model_space = "log1p";
    seed.sigma_init.value = 0.25;
    seed.uncertainty_init.available = true;
    seed.uncertainty_init.band_z = 6.0;
    seed.uncertainty_init.coverage_ratio = 0.9;
    seed.maturity_init.available = true;
    seed.maturity_init.seed_status = BootstrapSeedStatus::kFull;
    seed.maturity_init.confidence = 0.9;
    seed.maturity_init.accepted_count = 20000;
    seed.maturity_init.coverage_ratio = 0.9;
    seed.monthpos_hint.available = true;
    seed.monthpos_hint.dom_coeff.assign(31, 0.0);
    seed.monthpos_hint.dom_center.assign(31, 0.0);
    seed.monthpos_hint.dom_coeff[0] = 0.7;
    seed.monthpos_hint.dme_coeff.assign(8, 0.0);
    seed.monthpos_hint.dme_center.assign(8, 0.0);
    seed.monthpos_hint.lwd_coeff.assign(7, 0.0);
    seed.monthpos_hint.lwd_center.assign(7, 0.0);
    return seed;
}

void TestB3StateDefaultsAndSeedMapping() {
    const BaselineRollingConfig config = BuildConfig();
    RollingState empty;
    assert(BuildEmptyRollingState("link-a", config, &empty) == BaselineStatus::kOk);
    assert(empty.maturity_status == RollingMaturityStatus::kColdLearning);
    assert(empty.score_trust_status == ScoreTrustStatus::kScoreUntrusted);
    assert(empty.calibration_status == RollingCalibrationStatus::kUncalibrated);
    AssertNear(empty.detection_band_multiplier, 1.0);
    AssertNear(empty.coverage_ewma, 1.0);
    assert(empty.monthpos_status == RollingMonthposStatus::kDisabled);

    RollingState seeded;
    std::string diagnostics;
    assert(InitializeRollingStateFromBootstrapSeed(
               BuildSpec(), "link-a", BuildSeed(), config, &seeded, &diagnostics) ==
           BaselineStatus::kOk);
    assert(seeded.maturity_status == RollingMaturityStatus::kWeeklyReady);
    assert(seeded.score_trust_status == ScoreTrustStatus::kScoreWarming);
    assert(seeded.calibration_status == RollingCalibrationStatus::kWarming);
    AssertNear(seeded.detection_band_multiplier, 6.0 / config.band_z);
    assert(seeded.monthpos_status == RollingMonthposStatus::kMonthlyWarming);
    AssertNear(seeded.monthpos_dom_coeff[0], 0.7);

    assert(UpdateMaturityEvidence(BuildPoint(20001, 10.0), config, &seeded) ==
           BaselineStatus::kOk);
    assert(seeded.maturity_status == RollingMaturityStatus::kWeeklyReady);
}

void TestDetectionBandUsesCalibratedSigmaWithoutDoubleCounting() {
    const BaselineRollingConfig config = BuildConfig();
    RollingState state;
    assert(BuildEmptyRollingState("link-a", config, &state) == BaselineStatus::kOk);
    state.detection_band_multiplier = 2.0;
    state.sigma = 2.0;
    state.maturity_status = RollingMaturityStatus::kWeeklyReady;
    state.score_trust_status = ScoreTrustStatus::kScoreReady;

    ObservedModelPoint point = BuildPoint(11, 11.0);
    point.extra_obs_noise_scale = 3.0;
    RollingEstimatorResult estimator;
    estimator.status = BaselineStatus::kOk;
    estimator.model_mu = 10.0;
    estimator.pred_var = 4.0;
    estimator.obs_var = 4.0 + 3.0 * 4.0 + 4.0 + 0.25;
    estimator.residual = 1.0;

    DetectionBandResult band;
    BaselineRollingConfig uncapped_config = config;
    uncapped_config.detection_band_std_cap = 10.0;
    assert(BuildDetectionBand(state, point, estimator, config, 0.0, &band) ==
           BaselineStatus::kOk);
    AssertNear(band.raw_detection_var, 32.25);
    AssertNear(band.pred_var_component, 4.0);
    AssertNear(band.calibrated_sigma_var, 16.0);
    AssertNear(band.extra_obs_var, 12.25);
    AssertNear(band.maturity_uncertainty_var, 0.0);
    AssertNear(band.component_missing_uncertainty_var, 0.0);
    assert(band.std_cap_applied);
    AssertNear(band.detection_var, config.detection_band_std_cap * config.detection_band_std_cap);
    AssertNear(band.band_std, config.detection_band_std_cap);
    AssertNear(band.detection_z, 1.0 / config.detection_band_std_cap);
    assert(BuildDetectionBand(state, point, estimator, uncapped_config, 0.0, &band) ==
           BaselineStatus::kOk);
    AssertNear(band.raw_detection_var, 32.25);
    assert(!band.std_cap_applied);
    AssertNear(band.detection_var, 4.0 + 16.0 + 12.0 + 0.25);
    AssertNear(band.band_std, std::sqrt(32.25));
    AssertNear(band.detection_z, 1.0 / std::sqrt(32.25));

    state.maturity_status = RollingMaturityStatus::kColdLearning;
    state.score_trust_status = ScoreTrustStatus::kScoreUntrusted;
    assert(BuildDetectionBand(state, point, estimator, uncapped_config, 0.0, &band) ==
           BaselineStatus::kOk);
    assert(band.maturity_uncertainty_var > 0.0);
    assert(band.component_missing_uncertainty_var > 0.0);
    AssertNear(band.raw_detection_var, 32.25);
    AssertNear(band.detection_var, 32.25);
    AssertNear(band.band_std, std::sqrt(32.25));
}

void TestCalibrationUsesResidualScaleEstimate() {
    BaselineRollingConfig config = BuildConfig();
    config.calibration_alpha = 0.5;

    RollingState state;
    assert(BuildEmptyRollingState("link-a", config, &state) == BaselineStatus::kOk);
    state.calibration_update_count = config.calibration_warmup_min_updates;
    state.calibration_status = RollingCalibrationStatus::kWarming;
    state.score_trust_status = ScoreTrustStatus::kScoreWarming;
    state.detection_band_multiplier = 1.0;
    state.residual_scale_ewma = 1.0;
    state.sigma = 1.0;

    ObservedModelPoint point = BuildPoint(12, 3.0);
    RollingEstimatorResult estimator;
    estimator.status = BaselineStatus::kOk;
    estimator.model_mu = 0.0;
    estimator.pred_var = 0.0;

    DetectionBandResult band;
    BaselineRollingConfig uncapped_config = config;
    uncapped_config.detection_band_std_cap = 10.0;
    assert(BuildDetectionBand(state, point, estimator, uncapped_config, 0.0, &band) ==
           BaselineStatus::kOk);
    AssertNear(band.raw_calibration_var, 1.25);
    AssertNear(band.raw_z, 3.0 / std::sqrt(1.25));
    assert(UpdateDetectionCalibration(point, band, config, &state) == BaselineStatus::kOk);
    const double expected_scale = std::sqrt(0.5 * 1.0 + 0.5 * band.raw_z * band.raw_z);
    AssertNear(state.residual_scale_ewma, expected_scale * expected_scale);
    AssertNear(state.detection_band_multiplier, expected_scale);
    assert(state.calibration_status == RollingCalibrationStatus::kExpanding);

    state.calibration_update_count = config.score_ready_min_updates;
    state.score_trust_status = ScoreTrustStatus::kScoreReady;
    const double before = state.detection_band_multiplier;
    point = BuildPoint(13, 0.2);
    assert(BuildDetectionBand(state, point, estimator, uncapped_config, 0.0, &band) ==
           BaselineStatus::kOk);
    assert(UpdateDetectionCalibration(point, band, config, &state) ==
           BaselineStatus::kOk);
    assert(state.detection_band_multiplier < before);
    assert(state.calibration_status == RollingCalibrationStatus::kCalibrated);
}

void TestScoreTrustDegradesAndRecovers() {
    BaselineRollingConfig config = BuildConfig();
    RollingState state;
    assert(BuildEmptyRollingState("link-a", config, &state) == BaselineStatus::kOk);
    state.maturity_status = RollingMaturityStatus::kDailyReady;
    state.calibration_status = RollingCalibrationStatus::kCalibrated;
    state.calibration_update_count = config.score_ready_min_updates;
    state.coverage_ewma = 1.0;
    state.tail3_ewma = 0.0;
    state.tail5_ewma = 0.0;
    state.drift_evidence = 2.0;

    ScoreTrustResult result;
    assert(UpdateScoreTrust(BuildPoint(12, 10.0), 0.2, config, &state, &result) ==
           BaselineStatus::kOk);
    assert(state.score_trust_status == ScoreTrustStatus::kDriftLearning);
    assert(!result.can_alert);

    state.drift_evidence = 0.0;
    assert(UpdateScoreTrust(BuildPoint(13, 10.0), 0.2, config, &state, &result) ==
           BaselineStatus::kOk);
    assert(state.score_trust_status == ScoreTrustStatus::kRecalibrating);
    assert(!result.can_alert);

    assert(UpdateScoreTrust(BuildPoint(14, 10.0), 0.2, config, &state, &result) ==
           BaselineStatus::kOk);
    assert(UpdateScoreTrust(BuildPoint(15, 10.0), 0.2, config, &state, &result) ==
           BaselineStatus::kOk);
    assert(state.score_trust_status == ScoreTrustStatus::kScoreReady);
    assert(result.can_alert);
}

void TestScoreTrustDegradesOnLevelShiftEvidence() {
    BaselineRollingConfig config = BuildConfig();
    RollingState state;
    assert(BuildEmptyRollingState("link-a", config, &state) == BaselineStatus::kOk);
    state.maturity_status = RollingMaturityStatus::kDailyReady;
    state.calibration_status = RollingCalibrationStatus::kCalibrated;
    state.calibration_update_count = config.score_ready_min_updates;
    state.coverage_ewma = 1.0;
    state.tail3_ewma = 0.0;
    state.tail5_ewma = 0.0;
    state.drift_evidence = 0.0;
    state.level_shift_evidence = config.score_drift_degrade_start + 0.1;

    ScoreTrustResult result;
    assert(UpdateScoreTrust(BuildPoint(20, 10.0), 0.2, config, &state, &result) ==
           BaselineStatus::kOk);
    assert(state.score_trust_status == ScoreTrustStatus::kDriftLearning);
    assert(!result.can_alert);
    assert(state.degradation_reason == "level_shift_learning");
}

void TestScoreTrustUnavailableClearsStaleReadyStatus() {
    BaselineRollingConfig config = BuildConfig();
    RollingState state;
    assert(BuildEmptyRollingState("link-a", config, &state) == BaselineStatus::kOk);
    state.maturity_status = RollingMaturityStatus::kDailyReady;
    state.score_trust_status = ScoreTrustStatus::kScoreReady;
    state.score_confidence = config.confidence_ready_hint_cap;
    state.effective_confidence = config.confidence_ready_hint_cap;
    state.degradation_reason.clear();

    ObservedModelPoint point = BuildPoint(30, 10.0);
    point.can_score = false;
    ScoreTrustResult result;
    assert(UpdateScoreTrust(point, 0.0, config, &state, &result) == BaselineStatus::kOk);
    assert(state.score_trust_status == ScoreTrustStatus::kScoreUntrusted);
    assert(state.degradation_reason == "score_unavailable");
    assert(!result.can_alert);
    assert(result.reason == "score_unavailable");
    assert(state.score_confidence == 0.0);
    assert(state.effective_confidence == 0.0);
}

void TestMaturityCoverageProgression() {
    BaselineRollingConfig config = BuildConfig();
    RollingState state;
    assert(BuildEmptyRollingState("link-a", config, &state) == BaselineStatus::kOk);
    state.accepted_update_count = config.level_ready_min_updates;
    state.sigma = 1.0;

    assert(UpdateMaturityEvidence(BuildPoint(0, 10.0), config, &state) ==
           BaselineStatus::kOk);
    assert(MaturityAtLeast(state.maturity_status, RollingMaturityStatus::kLevelReady));
    assert(!MaturityAtLeast(state.maturity_status, RollingMaturityStatus::kDailyReady));
    assert(UpdateMaturityEvidence(BuildPoint(360, 10.0), config, &state) ==
           BaselineStatus::kOk);
    assert(UpdateMaturityEvidence(BuildPoint(720, 10.0), config, &state) ==
           BaselineStatus::kOk);
    assert(UpdateMaturityEvidence(BuildPoint(1080, 10.0), config, &state) ==
           BaselineStatus::kOk);
    state.accepted_update_count = config.day_buckets;
    assert(UpdateMaturityEvidence(BuildPoint(1440, 10.0), config, &state) ==
           BaselineStatus::kOk);
    assert(state.maturity_status >= RollingMaturityStatus::kDailyReady);
}

void TestMonthposCenteredBasisAndSlowUpdate() {
    BaselineRollingConfig config = BuildConfig();
    RollingState state;
    assert(BuildEmptyRollingState("link-a", config, &state) == BaselineStatus::kOk);
    BootstrapSeed seed = BuildSeed();
    assert(InitializeRollingMonthposFromSeed(seed, config, &state) == BaselineStatus::kOk);

    const double effect = EvaluateRollingMonthpos(state, 0, config);
    AssertNear(effect, 0.7);
    state.monthpos_status = RollingMonthposStatus::kMonthlyWarming;
    state.sigma = 2.0;
    const double before = state.monthpos_dom_coeff[0];
    assert(UpdateRollingMonthpos(BuildPoint(0, 11.0), 1.0, 1.0, config, &state) ==
           BaselineStatus::kOk);
    assert(std::fabs(state.monthpos_dom_coeff[0] - before) <=
           config.monthpos_alpha * config.monthpos_delta_max_scale * state.sigma);
}

void TestMonthposUpdatesDmeAndLastWeekdayTerms() {
    BaselineRollingConfig config = BuildConfig();
    RollingState state;
    assert(BuildEmptyRollingState("link-a", config, &state) == BaselineStatus::kOk);
    BootstrapSeed seed = BuildSeed();
    assert(InitializeRollingMonthposFromSeed(seed, config, &state) == BaselineStatus::kOk);
    state.monthpos_status = RollingMonthposStatus::kMonthlyWarming;
    state.sigma = 2.0;

    const int64_t last_friday_july_2021_bucket = 1627603200 / 60;
    const double before_dme = state.monthpos_dme_coeff[1];
    const double before_lwd = state.monthpos_lwd_coeff[5];
    assert(UpdateRollingMonthpos(
               BuildPoint(last_friday_july_2021_bucket, 11.0), 1.0, 1.0, config, &state) ==
           BaselineStatus::kOk);
    assert(state.monthpos_dme_coeff[1] != before_dme);
    assert(state.monthpos_lwd_coeff[5] != before_lwd);
}

}  // namespace

int main() {
    TestB3StateDefaultsAndSeedMapping();
    TestDetectionBandUsesCalibratedSigmaWithoutDoubleCounting();
    TestCalibrationUsesResidualScaleEstimate();
    TestScoreTrustDegradesAndRecovers();
    TestScoreTrustDegradesOnLevelShiftEvidence();
    TestScoreTrustUnavailableClearsStaleReadyStatus();
    TestMaturityCoverageProgression();
    TestMonthposCenteredBasisAndSlowUpdate();
    TestMonthposUpdatesDmeAndLastWeekdayTerms();
    return 0;
}
