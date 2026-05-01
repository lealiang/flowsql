/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/model/readiness_helper.h"

#include <algorithm>

namespace flowsql {
namespace baseline {

namespace {

double ComputeCoverage(uint64_t valid_bucket_count, uint64_t total_bucket_span) {
    if (total_bucket_span == 0) return 0.0;
    return static_cast<double>(valid_bucket_count) / static_cast<double>(total_bucket_span);
}

double ClipUnit(double value) {
    return std::max(0.0, std::min(1.0, value));
}

bool EvaluateMonthPosEligibility(const ReadinessState& state, const SharedProfileConfig& config) {
    return state.coverage_stats.month_count >= config.m_month_enable &&
           state.coverage_stats.coverage >= config.month_cov_min;
}

double ComputeBootstrapConfidenceBase(const ReadinessState& state,
                                      ModelReadiness readiness) {
    if (readiness == ModelReadiness::kNotReady) return 0.0;

    double confidence = readiness == ModelReadiness::kMonthposReady ? 1.0 : 0.8;
    if (state.coverage_degraded) {
        confidence *= 0.7;
    }
    return ClipUnit(confidence);
}

}  // namespace

ReadinessState BuildTrainReadiness(const TrainingCoverageStats& stats,
                                   const SharedProfileConfig& config) {
    ReadinessState state;
    state.coverage_stats.initialized = stats.total_bucket_span > 0 || stats.valid_bucket_count > 0;
    state.coverage_stats.valid_bucket_count = stats.valid_bucket_count;
    state.coverage_stats.total_bucket_span = stats.total_bucket_span;
    if (state.coverage_stats.total_bucket_span == 0 && stats.last_bucket_id >= stats.first_bucket_id) {
        state.coverage_stats.total_bucket_span =
            static_cast<uint64_t>(stats.last_bucket_id - stats.first_bucket_id + 1);
    }
    state.coverage_stats.first_bucket_id = stats.first_bucket_id;
    state.coverage_stats.last_bucket_id = stats.last_bucket_id;
    state.coverage_stats.month_count = stats.month_count;
    state.coverage_stats.coverage =
        ComputeCoverage(state.coverage_stats.valid_bucket_count, state.coverage_stats.total_bucket_span);
    state.coverage_degraded =
        state.coverage_stats.initialized && state.coverage_stats.coverage < config.month_cov_min;
    state.monthpos_enabled = EvaluateMonthPosEligibility(state, config);
    state.readiness = state.monthpos_enabled ? ModelReadiness::kMonthposReady
                                             : (stats.valid_bucket_count > 0
                                                    ? ModelReadiness::kCoreNoMonthReady
                                                    : ModelReadiness::kNotReady);
    state.confidence_base = ComputeBootstrapConfidenceBase(state, state.readiness);
    return state;
}

}  // namespace baseline
}  // namespace flowsql
