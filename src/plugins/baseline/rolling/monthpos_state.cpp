/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/rolling/monthpos_state.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <vector>

#include "plugins/baseline/model/calendar_feature_helper.h"

namespace flowsql {
namespace baseline {
namespace {

void CopyOrFill(const std::vector<double>& source, std::size_t size, std::vector<double>* out) {
    if (!out) return;
    out->assign(size, 0.0);
    const std::size_t n = std::min(size, source.size());
    for (std::size_t i = 0; i < n; ++i) (*out)[i] = source[i];
}

double CenteredOneHotEffect(const std::vector<double>& coeff,
                            const std::vector<double>& center,
                            std::size_t active) {
    if (coeff.empty()) return 0.0;
    double value = 0.0;
    for (std::size_t i = 0; i < coeff.size(); ++i) {
        const double basis = (i == active ? 1.0 : 0.0) -
                             (i < center.size() ? center[i] : 0.0);
        value += coeff[i] * basis;
    }
    return value;
}

std::size_t DayOfMonthIndex(int64_t bucket_id, const BaselineRollingConfig& config) {
    const int32_t dom = DayOfMonthLocal(bucket_id, config.bucket_seconds, config.timezone);
    if (dom < 1) return 0;
    return static_cast<std::size_t>(std::min(31, dom) - 1);
}

std::size_t DaysToMonthEndIndex(int64_t bucket_id,
                                const BaselineRollingConfig& config,
                                std::size_t size) {
    if (size == 0) return 0;
    const int32_t dme = DaysToMonthEndLocal(bucket_id, config.bucket_seconds, config.timezone);
    return static_cast<std::size_t>(
        std::max<int32_t>(0, std::min<int32_t>(static_cast<int32_t>(size - 1), dme)));
}

std::size_t LastWeekdayIndex(int64_t bucket_id, const BaselineRollingConfig& config) {
    std::tm local{};
    if (!ResolveLocalTime(bucket_id, config.bucket_seconds, config.timezone, &local)) return 0;
    return static_cast<std::size_t>(std::max(0, std::min(6, local.tm_wday)));
}

std::size_t DayOfMonthIndexFromFeature(const LocalCalendarFeature& feature) {
    if (!feature.valid || feature.day_of_month < 1) return 0;
    return static_cast<std::size_t>(std::min(31, feature.day_of_month) - 1);
}

std::size_t DaysToMonthEndIndexFromFeature(const LocalCalendarFeature& feature,
                                           std::size_t size) {
    if (!feature.valid || size == 0) return 0;
    return static_cast<std::size_t>(
        std::max<int32_t>(
            0,
            std::min<int32_t>(
                static_cast<int32_t>(size - 1), feature.days_to_month_end)));
}

std::size_t LastWeekdayIndexFromFeature(const LocalCalendarFeature& feature) {
    if (!feature.valid) return 0;
    return static_cast<std::size_t>(std::max(0, std::min(6, feature.weekday)));
}

double Clip(double value, double limit) {
    return std::max(-limit, std::min(limit, value));
}

void UpdateCenteredActiveCoeff(std::size_t active,
                               double alpha,
                               double delta,
                               std::vector<double>* coeff,
                               const std::vector<double>& center) {
    if (!coeff || active >= coeff->size()) return;
    const double center_value = active < center.size() ? center[active] : 0.0;
    const double basis = 1.0 - center_value;
    (*coeff)[active] = (1.0 - alpha * basis * basis) * (*coeff)[active] +
                       alpha * basis * delta;
}

}  // namespace

BaselineStatus InitializeRollingMonthposFromSeed(const BootstrapSeed& seed,
                                                 const BaselineRollingConfig&,
                                                 RollingState* state) {
    if (!state) return BaselineStatus::kInvalidArgument;
    if (!seed.monthpos_hint.available) return BaselineStatus::kInsufficientData;

    CopyOrFill(seed.monthpos_hint.dom_coeff, 31, &state->monthpos_dom_coeff);
    CopyOrFill(seed.monthpos_hint.dom_center, 31, &state->monthpos_dom_center);
    const std::size_t dme_size =
        std::max<std::size_t>(1, std::max(seed.monthpos_hint.dme_coeff.size(),
                                         seed.monthpos_hint.dme_center.size()));
    CopyOrFill(seed.monthpos_hint.dme_coeff, dme_size, &state->monthpos_dme_coeff);
    CopyOrFill(seed.monthpos_hint.dme_center, dme_size, &state->monthpos_dme_center);
    CopyOrFill(seed.monthpos_hint.lwd_coeff, 7, &state->monthpos_lwd_coeff);
    CopyOrFill(seed.monthpos_hint.lwd_center, 7, &state->monthpos_lwd_center);
    state->monthpos_dme_count.assign(std::max<std::size_t>(1, state->monthpos_dme_coeff.size()),
                                     0);
    state->monthpos_lwd_count.fill(0);
    state->monthpos_lwd_update_count = 0;
    state->monthpos_status = RollingMonthposStatus::kMonthlyWarming;
    return BaselineStatus::kOk;
}

double EvaluateRollingMonthpos(const RollingState& state,
                               int64_t bucket_id,
                               const BaselineRollingConfig& config) {
    double effect = CenteredOneHotEffect(
        state.monthpos_dom_coeff, state.monthpos_dom_center, DayOfMonthIndex(bucket_id, config));
    effect += CenteredOneHotEffect(state.monthpos_dme_coeff,
                                   state.monthpos_dme_center,
                                   DaysToMonthEndIndex(
                                       bucket_id, config, state.monthpos_dme_coeff.size()));
    if (IsLastWeekdayOfMonthLocal(bucket_id, config.bucket_seconds, config.timezone)) {
        effect += CenteredOneHotEffect(state.monthpos_lwd_coeff,
                                       state.monthpos_lwd_center,
                                       LastWeekdayIndex(bucket_id, config));
    }
    return effect;
}

double EvaluateRollingMonthposWithFeature(const RollingState& state,
                                          const LocalCalendarFeature& feature) {
    double effect = CenteredOneHotEffect(
        state.monthpos_dom_coeff,
        state.monthpos_dom_center,
        DayOfMonthIndexFromFeature(feature));
    effect += CenteredOneHotEffect(
        state.monthpos_dme_coeff,
        state.monthpos_dme_center,
        DaysToMonthEndIndexFromFeature(feature, state.monthpos_dme_coeff.size()));
    if (feature.valid && feature.is_last_weekday_of_month) {
        effect += CenteredOneHotEffect(state.monthpos_lwd_coeff,
                                       state.monthpos_lwd_center,
                                       LastWeekdayIndexFromFeature(feature));
    }
    return effect;
}

BaselineStatus UpdateRollingMonthpos(const ObservedModelPoint& point,
                                     double monthpos_residual,
                                     double update_weight,
                                     const BaselineRollingConfig& config,
                                     RollingState* state) {
    if (!state || point.status != BaselineStatus::kOk || !std::isfinite(monthpos_residual)) {
        return BaselineStatus::kInvalidArgument;
    }
    if (!point.can_update || update_weight <= 0.0 ||
        state->monthpos_status == RollingMonthposStatus::kDisabled) {
        return BaselineStatus::kOk;
    }
    const double sigma = std::max(config.sigma_floor, state->sigma);
    const double delta = Clip(monthpos_residual, config.monthpos_delta_max_scale * sigma);
    const double alpha = config.monthpos_alpha * std::max(0.0, std::min(1.0, update_weight));

    const std::size_t dom = DayOfMonthIndex(point.bucket_id, config);
    UpdateCenteredActiveCoeff(
        dom, alpha, delta, &state->monthpos_dom_coeff, state->monthpos_dom_center);
    const std::size_t dme =
        DaysToMonthEndIndex(point.bucket_id, config, state->monthpos_dme_coeff.size());
    UpdateCenteredActiveCoeff(
        dme, alpha, delta, &state->monthpos_dme_coeff, state->monthpos_dme_center);
    if (IsLastWeekdayOfMonthLocal(point.bucket_id, config.bucket_seconds, config.timezone)) {
        UpdateCenteredActiveCoeff(LastWeekdayIndex(point.bucket_id, config),
                                  alpha,
                                  delta,
                                  &state->monthpos_lwd_coeff,
                                  state->monthpos_lwd_center);
    }
    state->monthpos_update_count += 1;
    return BaselineStatus::kOk;
}

}  // namespace baseline
}  // namespace flowsql
