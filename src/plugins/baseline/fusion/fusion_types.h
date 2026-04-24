/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_FUSION_FUSION_TYPES_H_
#define _FLOWSQL_PLUGINS_BASELINE_FUSION_FUSION_TYPES_H_

#include <algorithm>
#include <string>
#include <vector>

#include <framework/interfaces/ibaseline_types.h>

namespace flowsql {
namespace baseline {

struct StoredDominantSingleProjection {
    std::string feature;
    BaselineDirection dir = BaselineDirection::kUnknown;
    BaselineReasonCode reason_code = BaselineReasonCode::kUnknown;
    double a_f = 0.0;
    double normalized_score = 0.0;
    double confidence = 0.0;
    uint32_t persistence = 0;
};

struct StoredDominantPatternProjection {
    std::string pattern;
    std::string feature_base;
    double score_pattern = 0.0;
    double weighted_score = 0.0;
    std::vector<std::string> metrics_hit;
    std::vector<std::string> supporting_features;
};

struct StoredFusionResult {
    bool available = false;
    std::string key;
    int64_t ts = 0;
    double risk = 0.0;
    std::vector<StoredDominantSingleProjection> dominant_singles;
    std::vector<StoredDominantPatternProjection> dominant_patterns;
};

inline BaselineStringRef MakeOwnedStringRef(const std::string& value) {
    return BaselineStringRef{value.empty() ? nullptr : value.c_str(),
                             static_cast<uint32_t>(value.size())};
}

inline void MaterializeStoredFusionResult(const StoredFusionResult& stored,
                                          FusionResult* out) {
    if (!out) return;

    *out = FusionResult{};
    if (!stored.available) return;

    out->key = MakeOwnedStringRef(stored.key);
    out->ts = stored.ts;
    out->risk = stored.risk;

    const uint32_t single_count = std::min<uint32_t>(
        static_cast<uint32_t>(stored.dominant_singles.size()),
        kBaselineDominantSingleLimit);
    out->dominant_single_count = single_count;
    for (uint32_t i = 0; i < single_count; ++i) {
        const auto& src = stored.dominant_singles[i];
        auto& dst = out->dominant_single[i];
        dst.feature = MakeOwnedStringRef(src.feature);
        dst.dir = src.dir;
        dst.reason_code = src.reason_code;
        dst.a_f = src.a_f;
        dst.normalized_score = src.normalized_score;
        dst.confidence = src.confidence;
        dst.persistence = src.persistence;
    }

    const uint32_t pattern_count = std::min<uint32_t>(
        static_cast<uint32_t>(stored.dominant_patterns.size()),
        kBaselineDominantPatternLimit);
    out->dominant_pattern_count = pattern_count;
    for (uint32_t i = 0; i < pattern_count; ++i) {
        const auto& src = stored.dominant_patterns[i];
        auto& dst = out->dominant_pattern[i];
        dst.pattern = MakeOwnedStringRef(src.pattern);
        dst.feature_base = MakeOwnedStringRef(src.feature_base);
        dst.score_pattern = src.score_pattern;

        const uint32_t metric_count = std::min<uint32_t>(
            static_cast<uint32_t>(src.metrics_hit.size()),
            kBaselinePatternMetricsHitLimit);
        dst.metrics_hit_count = metric_count;
        for (uint32_t j = 0; j < metric_count; ++j) {
            dst.metrics_hit[j] = MakeOwnedStringRef(src.metrics_hit[j]);
        }

        const uint32_t support_count = std::min<uint32_t>(
            static_cast<uint32_t>(src.supporting_features.size()),
            kBaselinePatternSupportingFeatureLimit);
        dst.supporting_feature_count = support_count;
        for (uint32_t j = 0; j < support_count; ++j) {
            dst.supporting_features[j] = MakeOwnedStringRef(src.supporting_features[j]);
        }
    }
}

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_FUSION_FUSION_TYPES_H_
