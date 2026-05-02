/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/rolling/maturity_gate.h"

#include <algorithm>
#include <ctime>
#include <limits>

#include "plugins/baseline/model/calendar_feature_helper.h"

namespace flowsql {
namespace baseline {
namespace {

uint32_t PositiveModulo(int64_t value, uint32_t mod) {
    if (mod == 0) return 0;
    const int64_t r = value % static_cast<int64_t>(mod);
    return static_cast<uint32_t>(r < 0 ? r + static_cast<int64_t>(mod) : r);
}

uint32_t PhaseBin(int64_t bucket_id, uint64_t period_buckets, uint32_t bins) {
    if (period_buckets == 0 || bins == 0) return 0;
    const uint64_t phase =
        static_cast<uint64_t>(PositiveModulo(bucket_id, static_cast<uint32_t>(period_buckets)));
    return static_cast<uint32_t>(
        std::min<uint64_t>(bins - 1, phase * static_cast<uint64_t>(bins) / period_buckets));
}

double RatioNonZero(const std::vector<uint32_t>& bins) {
    if (bins.empty()) return 0.0;
    uint64_t non_zero = 0;
    for (uint32_t value : bins) {
        if (value > 0) ++non_zero;
    }
    return static_cast<double>(non_zero) / static_cast<double>(bins.size());
}

double RatioNonZero(const std::array<uint32_t, 31>& bins) {
    uint64_t non_zero = 0;
    for (uint32_t value : bins) {
        if (value > 0) ++non_zero;
    }
    return static_cast<double>(non_zero) / 31.0;
}

double RatioNonZero(const std::array<uint32_t, 7>& bins) {
    uint64_t non_zero = 0;
    for (uint32_t value : bins) {
        if (value > 0) ++non_zero;
    }
    return static_cast<double>(non_zero) / 7.0;
}

uint64_t MaturitySupportCount(const RollingState& state) {
    if (std::numeric_limits<uint64_t>::max() - state.accepted_update_count <
        state.maturity_prior_update_count) {
        return std::numeric_limits<uint64_t>::max();
    }
    return state.accepted_update_count + state.maturity_prior_update_count;
}

int32_t MonthId(int64_t bucket_id, const BaselineRollingConfig& config) {
    std::tm local{};
    if (!ResolveLocalTime(bucket_id, config.bucket_seconds, config.timezone, &local)) return 0;
    return (local.tm_year + 1900) * 12 + local.tm_mon;
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

bool MonthposLwdReady(const RollingState& state, const BaselineRollingConfig& config) {
    return state.monthpos_lwd_update_count >= config.monthpos_min_month_transitions;
}

RollingMaturityStatus ComputeMaturity(const RollingState& state,
                                      const BaselineRollingConfig& config) {
    const uint64_t maturity_support_count = MaturitySupportCount(state);
    if (maturity_support_count < config.level_ready_min_updates) {
        return RollingMaturityStatus::kColdLearning;
    }

    RollingMaturityStatus status = RollingMaturityStatus::kLevelReady;
    const uint64_t observed_days =
        maturity_support_count / std::max<uint64_t>(1, config.day_buckets);
    if (DailyCoverageRatio(state) > 0.0) status = RollingMaturityStatus::kDailyWarming;
    if (observed_days >= config.daily_ready_min_days &&
        DailyCoverageRatio(state) >= config.daily_ready_coverage_ratio) {
        status = RollingMaturityStatus::kDailyReady;
    } else {
        return status;
    }

    const uint64_t observed_weeks =
        maturity_support_count / std::max<uint64_t>(1, config.week_buckets);
    if (WeeklyCoverageRatio(state) > 0.0) status = RollingMaturityStatus::kWeeklyWarming;
    if (observed_weeks >= config.weekly_ready_min_weeks &&
        WeeklyCoverageRatio(state) >= config.weekly_ready_coverage_ratio) {
        status = RollingMaturityStatus::kWeeklyReady;
    } else {
        return status;
    }

    if (state.month_transition_count > 0 || state.monthpos_update_count > 0) {
        status = RollingMaturityStatus::kMonthlyWarming;
    }
    if (state.month_transition_count >= config.monthpos_min_month_transitions &&
        MonthposCoverageRatio(state) >= config.monthpos_ready_coverage_ratio &&
        MonthposLwdReady(state, config) &&
        state.score_trust_status != ScoreTrustStatus::kDriftLearning) {
        status = RollingMaturityStatus::kMonthlyReady;
    }
    return status;
}

void UpdateLegacyConfidence(const BaselineRollingConfig& config, RollingState* state) {
    if (!state) return;
    if (MaturityAtLeast(state->maturity_status, RollingMaturityStatus::kWeeklyReady)) {
        state->learning_confidence = config.confidence_ready_hint_cap;
    } else if (MaturityAtLeast(state->maturity_status, RollingMaturityStatus::kLevelReady)) {
        state->learning_confidence = config.confidence_warming;
    } else {
        state->learning_confidence = config.confidence_cold;
    }
    state->confidence = state->learning_confidence;
    state->effective_confidence =
        std::min(state->learning_confidence, state->score_confidence);
}

}  // namespace

BaselineStatus UpdateMaturityEvidence(const ObservedModelPoint& point,
                                      const BaselineRollingConfig& config,
                                      RollingState* state) {
    if (!state || point.status != BaselineStatus::kOk) return BaselineStatus::kInvalidArgument;
    if (!point.can_update) return BaselineStatus::kOk;

    if (state->daily_bin_count.empty()) {
        state->daily_bin_count.assign(std::max<uint32_t>(1, config.daily_coverage_bins), 0);
    }
    if (state->weekly_bin_count.empty()) {
        state->weekly_bin_count.assign(std::max<uint32_t>(1, config.weekly_coverage_bins), 0);
    }

    state->daily_bin_count[PhaseBin(point.bucket_id,
                                    std::max<uint64_t>(1, config.day_buckets),
                                    static_cast<uint32_t>(state->daily_bin_count.size()))] += 1;
    state->weekly_bin_count[PhaseBin(point.bucket_id,
                                     std::max<uint64_t>(1, config.week_buckets),
                                     static_cast<uint32_t>(state->weekly_bin_count.size()))] += 1;

    const int32_t dom =
        DayOfMonthLocal(point.bucket_id, config.bucket_seconds, config.timezone);
    if (dom >= 1 && dom <= 31) state->monthpos_count[static_cast<std::size_t>(dom - 1)] += 1;
    if (state->monthpos_dme_count.empty()) {
        state->monthpos_dme_count.assign(
            std::max<std::size_t>(1, state->monthpos_dme_coeff.size()), 0);
    }
    state->monthpos_dme_count[DaysToMonthEndIndex(
        point.bucket_id, config, state->monthpos_dme_count.size())] += 1;
    if (IsLastWeekdayOfMonthLocal(point.bucket_id, config.bucket_seconds, config.timezone)) {
        state->monthpos_lwd_count[LastWeekdayIndex(point.bucket_id, config)] += 1;
        state->monthpos_lwd_update_count += 1;
    }

    const int32_t month_id = MonthId(point.bucket_id, config);
    if (month_id != 0) {
        if (state->last_seen_month_id != 0 && state->last_seen_month_id != month_id) {
            state->month_transition_count += 1;
        }
        state->last_seen_month_id = month_id;
    }

    state->maturity_status = ComputeMaturity(*state, config);
    if (state->maturity_status == RollingMaturityStatus::kMonthlyReady) {
        state->monthpos_status = RollingMonthposStatus::kMonthlyReady;
    } else if (state->maturity_status == RollingMaturityStatus::kMonthlyWarming) {
        state->monthpos_status = RollingMonthposStatus::kMonthlyWarming;
    } else if (state->monthpos_status == RollingMonthposStatus::kMonthlyReady) {
        state->monthpos_status = RollingMonthposStatus::kMonthlyWarming;
    }
    UpdateLegacyConfidence(config, state);
    return BaselineStatus::kOk;
}

std::vector<std::string> BuildEnabledComponents(const RollingState& state) {
    std::vector<std::string> out;
    if (MaturityAtLeast(state.maturity_status, RollingMaturityStatus::kLevelReady)) {
        out.push_back("level");
    }
    if (MaturityAtLeast(state.maturity_status, RollingMaturityStatus::kDailyReady)) {
        out.push_back("daily");
    }
    if (MaturityAtLeast(state.maturity_status, RollingMaturityStatus::kWeeklyReady)) {
        out.push_back("weekly");
    }
    if (state.monthpos_status == RollingMonthposStatus::kMonthlyReady) {
        out.push_back("monthpos");
    }
    return out;
}

std::vector<std::string> BuildComponentReadiness(const RollingState& state) {
    std::vector<std::string> out;
    out.push_back(MaturityAtLeast(state.maturity_status, RollingMaturityStatus::kLevelReady)
                      ? "level=ready"
                      : "level=warming:not_enough_updates");
    out.push_back(MaturityAtLeast(state.maturity_status, RollingMaturityStatus::kDailyReady)
                      ? "daily=ready"
                      : (MaturityAtLeast(state.maturity_status, RollingMaturityStatus::kDailyWarming)
                             ? "daily=warming:coverage_low"
                             : "daily=disabled:not_enough_days"));
    out.push_back(MaturityAtLeast(state.maturity_status, RollingMaturityStatus::kWeeklyReady)
                      ? "weekly=ready"
                      : (MaturityAtLeast(state.maturity_status, RollingMaturityStatus::kWeeklyWarming)
                             ? "weekly=warming:coverage_low"
                             : "weekly=disabled:not_enough_weeks"));
    out.push_back(state.monthpos_status == RollingMonthposStatus::kMonthlyReady
                      ? "monthpos=ready"
                      : (state.monthpos_status == RollingMonthposStatus::kMonthlyWarming
                             ? "monthpos=warming:not_enough_months"
                             : "monthpos=disabled:not_enough_months"));
    return out;
}

double DailyCoverageRatio(const RollingState& state) { return RatioNonZero(state.daily_bin_count); }

double WeeklyCoverageRatio(const RollingState& state) { return RatioNonZero(state.weekly_bin_count); }

double MonthposCoverageRatio(const RollingState& state) {
    const double dom_ratio = RatioNonZero(state.monthpos_count);
    const double dme_ratio =
        state.monthpos_dme_count.empty() ? 0.0 : RatioNonZero(state.monthpos_dme_count);
    return std::min(dom_ratio, dme_ratio);
}

}  // namespace baseline
}  // namespace flowsql
