/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_RELATION_RELATION_FUSION_H_
#define _FLOWSQL_PLUGINS_BASELINE_RELATION_RELATION_FUSION_H_

#include <framework/interfaces/ibaseline_types.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace flowsql {
namespace baseline {

struct RelationFusionRuntimeConfig {
    bool enable_relation_fusion = true;
    double fusion_z_score_cap = 5.0;
    double fusion_min_evidence_score = 0.20;
    uint32_t fusion_persistence_window = 2;
    double fusion_warming_weight = 0.25;
    double fusion_degraded_weight = 0.25;
    double fusion_support_weight = 0.5;
    double fusion_oppose_weight = 0.5;
    double basic_pattern_weight = 0.70;
    double stable_head_pattern_weight = 0.85;
    uint32_t dominant_single_cap = 3;
    uint32_t dominant_pattern_cap = 2;
};

struct RelationFusionMetricContext {
    std::string metric;
    bool present = false;
    bool valid = false;
    std::string unavailable_reason;
    bool active_count_from_upstream = false;
    bool has_active_basis = false;
    uint64_t basis_version = 0;
    bool stable_head_mix_drift_expected = false;
    uint32_t stable_head_size = 0;
};

struct RelationFusionRoutedInput {
    RelationRoutedSummaryResult routed;
    std::string feature_base;
    std::string metric_basis_status;
    bool active_count_from_upstream = false;
};

struct RelationFusionUpdateInput {
    std::string source_series_key;
    std::string feature_base;
    int64_t bucket_id = 0;
    RelationFusionRuntimeConfig config;
    std::vector<RelationFusionMetricContext> metrics;
    std::vector<RelationFusionRoutedInput> routed_inputs;
};

struct RelationFusionRuntimeState {
    int64_t last_bucket_id = 0;
    bool has_last_bucket = false;
    std::unordered_map<std::string, uint32_t> persistence_by_evidence_dir;
    RelationFusionResult last_result;
};

BaselineStatus UpdateRelationFusion(const RelationFusionUpdateInput& input,
                                    RelationFusionRuntimeState* state,
                                    RelationFusionResult* out);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_RELATION_RELATION_FUSION_H_
