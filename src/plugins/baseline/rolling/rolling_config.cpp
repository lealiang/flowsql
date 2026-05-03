/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/rolling/rolling_config.h"

#include <algorithm>
#include <utility>

#include "plugins/baseline/config/runtime_config.h"
#include "plugins/baseline/model/profile_config.h"

namespace flowsql {
namespace baseline {
namespace {

uint64_t CeilDiv(int64_t numerator, int64_t denominator) {
    if (numerator <= 0 || denominator <= 0) return 0;
    return static_cast<uint64_t>((numerator + denominator - 1) / denominator);
}

bool Positive(double value) { return value > 0.0; }
bool NonNegative(double value) { return value >= 0.0; }
bool UnitClosed(double value) { return value >= 0.0 && value <= 1.0; }
bool UnitOpenClosed(double value) { return value > 0.0 && value <= 1.0; }
bool UnitClosedOpen(double value) { return value >= 0.0 && value < 1.0; }

BaselineStatus Invalid(std::string* err, const char* message) {
    if (err) *err = message ? message : "invalid rolling config";
    return BaselineStatus::kInvalidArgument;
}

}  // namespace

BaselineRollingConfig DefaultBaselineRollingConfig() {
    BaselineRollingConfig config;
    return config;
}

BaselineStatus ValidateBaselineRollingConfig(const BaselineRollingConfig& config,
                                             std::string* err) {
    if (config.n_min_score == 0 ||
        config.n_min_update < config.n_min_score ||
        config.n_ref == 0) {
        return Invalid(err, "rolling_config sampled value thresholds are invalid");
    }
    if (config.d_min_score == 0 ||
        config.d_min_update < config.d_min_score ||
        config.d_ref == 0) {
        return Invalid(err, "rolling_config ratio denominator thresholds are invalid");
    }
    if (!Positive(config.sample_count_noise) ||
        !Positive(config.ratio_denominator_noise)) {
        return Invalid(err, "rolling_config observation noise fields must be > 0");
    }
    if (!Positive(config.z_downweight) || config.z_skip <= config.z_downweight) {
        return Invalid(err, "rolling_config.z_skip must be > z_downweight > 0");
    }
    if (!UnitClosed(config.small_update_weight)) {
        return Invalid(err, "rolling_config.small_update_weight must be in [0,1]");
    }
    if (config.daily_harmonic_order < 0 || config.weekly_harmonic_order < 0) {
        return Invalid(err, "rolling_config harmonic orders must be >= 0");
    }
    if (!NonNegative(config.level_learning_scale) ||
        !NonNegative(config.day_learning_scale) ||
        !NonNegative(config.week_learning_scale) ||
        !NonNegative(config.cold_day_learning_scale) ||
        !NonNegative(config.cold_week_learning_scale) ||
        !UnitClosed(config.seasonal_drift_min_scale) ||
        !UnitClosed(config.full_seed_seasonal_scale) ||
        !UnitClosed(config.partial_seed_seasonal_scale)) {
        return Invalid(err, "rolling_config learning scales are invalid");
    }
    if (!Positive(config.day_delta_coeff_max_scale) ||
        !Positive(config.week_delta_coeff_max_scale) ||
        !Positive(config.trend_delta_max_scale) ||
        !Positive(config.trend_abs_max_scale)) {
        return Invalid(err, "rolling_config delta caps must be > 0");
    }
    if (!NonNegative(config.q_day_scale) ||
        !NonNegative(config.q_week_scale) ||
        !NonNegative(config.q_level_scale) ||
        !NonNegative(config.q_trend_scale)) {
        return Invalid(err, "rolling_config process noise scales must be >= 0");
    }
    if (!NonNegative(config.trend_update_scale) ||
        !NonNegative(config.cold_trend_update_scale)) {
        return Invalid(err, "rolling_config trend update scales must be >= 0");
    }
    if (!Positive(config.p_level_init_scale) ||
        !Positive(config.p_trend_init_scale) ||
        !Positive(config.p_day_init_scale) ||
        !Positive(config.p_week_init_scale) ||
        !Positive(config.p_floor_scale) ||
        !Positive(config.p_cap_scale) ||
        config.p_cap_scale <= config.p_floor_scale) {
        return Invalid(err, "rolling_config covariance scales are invalid");
    }
    if (!UnitOpenClosed(config.alpha_short) ||
        !UnitOpenClosed(config.alpha_long) ||
        config.alpha_long > config.alpha_short) {
        return Invalid(err, "rolling_config drift EWMA alpha values are invalid");
    }
    if (!Positive(config.z_cap) ||
        !NonNegative(config.drift_start) ||
        config.drift_full <= config.drift_start ||
        !NonNegative(config.level_shift_reference_z) ||
        !UnitClosedOpen(config.level_shift_cusum_decay) ||
        !Positive(config.level_shift_cusum_threshold) ||
        !NonNegative(config.max_level_boost) ||
        !NonNegative(config.max_q_boost) ||
        !NonNegative(config.skip_relax)) {
        return Invalid(err, "rolling_config drift fields are invalid");
    }
    if (!UnitOpenClosed(config.alpha_sigma) ||
        !Positive(config.c_sigma) ||
        !Positive(config.sigma_floor) ||
        !Positive(config.cold_start_band_scale) ||
        !Positive(config.band_z) ||
        !Positive(config.forecast_band_z)) {
        return Invalid(err, "rolling_config residual scale or band fields are invalid");
    }
    if (!UnitClosed(config.confidence_cold) ||
        !UnitClosed(config.confidence_warming) ||
        !UnitClosed(config.confidence_ready_hint_cap) ||
        config.confidence_cold > config.confidence_warming ||
        config.confidence_warming > config.confidence_ready_hint_cap) {
        return Invalid(err, "rolling_config confidence fields are invalid");
    }
    if (config.min_warming_updates == 0) {
        return Invalid(err, "rolling_config.min_warming_updates must be > 0");
    }
    if (config.level_ready_min_updates == 0 ||
        config.score_warming_min_updates == 0 ||
        config.score_ready_min_updates < config.score_warming_min_updates ||
        config.score_recovery_min_updates == 0 ||
        !NonNegative(config.score_drift_degrade_start)) {
        return Invalid(err, "rolling_config score trust thresholds are invalid");
    }
    if (!UnitOpenClosed(config.calibration_alpha) ||
        config.calibration_warmup_min_updates == 0 ||
        !UnitOpenClosed(config.calibration_coverage_floor) ||
        !NonNegative(config.calibration_tail3_limit) ||
        !NonNegative(config.calibration_tail5_limit) ||
        config.calibration_multiplier_min <= 0.0 ||
        config.calibration_multiplier_max < config.calibration_multiplier_min) {
        return Invalid(err, "rolling_config calibration fields are invalid");
    }
    if (config.daily_coverage_bins == 0 ||
        config.weekly_coverage_bins == 0 ||
        config.daily_ready_min_days == 0 ||
        config.weekly_ready_min_weeks == 0 ||
        !UnitOpenClosed(config.daily_ready_coverage_ratio) ||
        !UnitOpenClosed(config.weekly_ready_coverage_ratio)) {
        return Invalid(err, "rolling_config component readiness fields are invalid");
    }
    if (!NonNegative(config.maturity_uncertainty_cold_scale) ||
        !NonNegative(config.maturity_uncertainty_warming_scale) ||
        !NonNegative(config.maturity_uncertainty_drift_scale) ||
        !NonNegative(config.maturity_uncertainty_recalibrating_scale) ||
        !NonNegative(config.missing_daily_uncertainty_scale) ||
        !NonNegative(config.missing_weekly_uncertainty_scale) ||
        !Positive(config.level_only_extreme_z) ||
        !Positive(config.detection_band_std_cap)) {
        return Invalid(err, "rolling_config uncertainty scale fields are invalid");
    }
    if (!UnitOpenClosed(config.monthpos_alpha) ||
        !Positive(config.monthpos_delta_max_scale) ||
        config.monthpos_min_month_transitions == 0 ||
        !UnitOpenClosed(config.monthpos_ready_coverage_ratio)) {
        return Invalid(err, "rolling_config monthpos fields are invalid");
    }
    const auto& relation = config.relation_rolling;
    if (relation.basis_stats_max_groups == 0 ||
        !UnitOpenClosed(relation.basis_candidate_min_coverage_ratio) ||
        !NonNegative(relation.basis_replacement_cap_ratio) ||
        relation.basis_replacement_cap_max == 0 ||
        relation.basis_threshold_margin < 1.0 ||
        relation.basis_min_stable_refresh_count == 0 ||
        relation.routed_state_shard_count == 0) {
        return Invalid(err, "rolling_config.relation_rolling fields are invalid");
    }
    if (config.bucket_seconds <= 0 ||
        config.timezone.empty() ||
        config.day_buckets == 0 ||
        config.week_buckets == 0) {
        return Invalid(err, "rolling_config resolved time fields are invalid");
    }
    return BaselineStatus::kOk;
}

BaselineStatus ResolveBaselineRollingConfig(const BaselineTaskSpec& spec,
                                            BaselineRollingConfig* out,
                                            std::string* err) {
    if (!out) return Invalid(err, "rolling config output must not be null");

    BaselineRollingConfig config = DefaultBaselineRollingConfig();
    (void)TryGetBaselineRollingConfigOverride(&config);

    config.bucket_seconds =
        spec.clock_spec.bucket_seconds > 0 ? spec.clock_spec.bucket_seconds : spec.delta;
    if (config.bucket_seconds <= 0) {
        return Invalid(err, "task clock bucket_seconds must be > 0");
    }
    config.timezone = !spec.clock_spec.timezone.empty()
                          ? spec.clock_spec.timezone
                          : (!spec.tz.empty() ? spec.tz : BaselineDefaultTimezone());
    if (config.timezone.empty()) config.timezone = "UTC";

    config.day_buckets = std::max<uint64_t>(1, CeilDiv(86400, config.bucket_seconds));
    config.week_buckets = std::max<uint64_t>(1, CeilDiv(604800, config.bucket_seconds));
    if (config.process_noise_gap_cap_buckets == 0) {
        config.process_noise_gap_cap_buckets = 7 * config.day_buckets;
    }
    if (config.min_ready_hint_updates == 0) {
        config.min_ready_hint_updates = config.day_buckets;
    }
    if (config.forecast_trend_cap_buckets == 0) {
        config.forecast_trend_cap_buckets = config.day_buckets;
    }

    const BaselineStatus validation = ValidateBaselineRollingConfig(config, err);
    if (validation != BaselineStatus::kOk) return validation;
    *out = std::move(config);
    return BaselineStatus::kOk;
}

}  // namespace baseline
}  // namespace flowsql
