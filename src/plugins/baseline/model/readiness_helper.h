/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_MODEL_READINESS_HELPER_H_
#define _FLOWSQL_PLUGINS_BASELINE_MODEL_READINESS_HELPER_H_

#include <cstdint>

#include <framework/interfaces/ibaseline_types.h>

#include "plugins/baseline/model/profile_config.h"

namespace flowsql {
namespace baseline {

enum class ModelReadiness : int32_t {
    kNotReady = 0,
    kCoreNoMonthReady = 1,
    kMonthposReady = 2,
};

struct CoverageStats {
    bool initialized = false;
    uint64_t valid_bucket_count = 0;
    uint64_t total_bucket_span = 0;
    int64_t first_bucket_id = 0;
    int64_t last_bucket_id = 0;
    uint32_t month_count = 0;
    double coverage = 0.0;
};

struct TrainingCoverageStats {
    uint64_t valid_bucket_count = 0;
    uint64_t total_bucket_span = 0;
    int64_t first_bucket_id = 0;
    int64_t last_bucket_id = 0;
    uint32_t month_count = 0;
};

struct ReadinessState {
    CoverageStats coverage_stats;
    bool monthpos_enabled = false;
    ModelReadiness readiness = ModelReadiness::kNotReady;
    double confidence_base = 0.0;
    bool coverage_degraded = false;
};

void UpdateCoverageStats(ReadinessState* state, int64_t bucket_id, bool is_valid_bucket);
bool EvaluateMonthPosEligibility(const ReadinessState& state, const SharedProfileConfig& config);
double ComputeConfidenceBase(const ReadinessState& state,
                             ModelReadiness readiness,
                             BaselineSourceKind source_kind);
void RefreshOnlineReadiness(ReadinessState* state,
                            const SharedProfileConfig& config,
                            ModelReadiness readiness,
                            BaselineSourceKind source_kind);
ReadinessState BuildTrainReadiness(const TrainingCoverageStats& stats,
                                   const SharedProfileConfig& config);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_MODEL_READINESS_HELPER_H_
