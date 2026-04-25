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
#include <array>
#include <string>

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
    uint32_t metrics_hit_count = 0;
    std::array<std::string, kBaselinePatternMetricsHitLimit> metrics_hit;
    uint32_t supporting_feature_count = 0;
    std::array<std::string, kBaselinePatternSupportingFeatureLimit> supporting_features;
};

struct StoredFusionResult {
    bool available = false;
    std::string key;
    int64_t ts = 0;
    double risk = 0.0;
    uint32_t dominant_single_count = 0;
    std::array<StoredDominantSingleProjection, kBaselineDominantSingleLimit> dominant_singles;
    uint32_t dominant_pattern_count = 0;
    std::array<StoredDominantPatternProjection, kBaselineDominantPatternLimit> dominant_patterns;
};

inline BaselineStringRef MakeOwnedStringRef(const std::string& value) {
    return BaselineStringRef{value.empty() ? nullptr : value.c_str(),
                             static_cast<uint32_t>(value.size())};
}

inline void AppendStoredMetricHit(StoredDominantPatternProjection* projection,
                                  const std::string& metric) {
    if (!projection || projection->metrics_hit_count >= kBaselinePatternMetricsHitLimit) return;
    projection->metrics_hit[projection->metrics_hit_count++] = metric;
}

inline void AppendStoredSupportingFeature(StoredDominantPatternProjection* projection,
                                          const std::string& feature) {
    if (!projection ||
        projection->supporting_feature_count >= kBaselinePatternSupportingFeatureLimit) {
        return;
    }
    projection->supporting_features[projection->supporting_feature_count++] = feature;
}

inline void MaterializeStoredFusionResult(const StoredFusionResult& stored,
                                          FusionResult* out) {
    if (!out) return;

    *out = FusionResult{};
    if (!stored.available) return;

    out->key = MakeOwnedStringRef(stored.key);
    out->ts = stored.ts;
    out->risk = stored.risk;

    const uint32_t single_count =
        std::min<uint32_t>(stored.dominant_single_count, kBaselineDominantSingleLimit);
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

    const uint32_t pattern_count =
        std::min<uint32_t>(stored.dominant_pattern_count, kBaselineDominantPatternLimit);
    out->dominant_pattern_count = pattern_count;
    for (uint32_t i = 0; i < pattern_count; ++i) {
        const auto& src = stored.dominant_patterns[i];
        auto& dst = out->dominant_pattern[i];
        dst.pattern = MakeOwnedStringRef(src.pattern);
        dst.feature_base = MakeOwnedStringRef(src.feature_base);
        dst.score_pattern = src.score_pattern;

        const uint32_t metric_count =
            std::min<uint32_t>(src.metrics_hit_count, kBaselinePatternMetricsHitLimit);
        dst.metrics_hit_count = metric_count;
        for (uint32_t j = 0; j < metric_count; ++j) {
            dst.metrics_hit[j] = MakeOwnedStringRef(src.metrics_hit[j]);
        }

        const uint32_t support_count = std::min<uint32_t>(
            src.supporting_feature_count, kBaselinePatternSupportingFeatureLimit);
        dst.supporting_feature_count = support_count;
        for (uint32_t j = 0; j < support_count; ++j) {
            dst.supporting_features[j] = MakeOwnedStringRef(src.supporting_features[j]);
        }
    }
}

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_FUSION_FUSION_TYPES_H_
