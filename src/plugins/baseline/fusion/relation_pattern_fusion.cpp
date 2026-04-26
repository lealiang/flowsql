/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/fusion/relation_pattern_fusion.h"

#include <common/error_code.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "plugins/baseline/config/runtime_config.h"

namespace flowsql {
namespace baseline {

namespace {

struct EvidenceSignal {
    bool present = false;
    double a_f = 0.0;
    double a_up = 0.0;
    double a_down = 0.0;
    std::string feature;
    StoredDominantSingleProjection single_projection;
};

struct MetricEvidenceBundle {
    std::string metric_name;
    std::unordered_map<RelationSummaryKind, EvidenceSignal> by_summary;
};

struct MetricPatternSignal {
    bool available = false;
    double score = 0.0;
    std::vector<std::string> supporting_features;
};

double ClipUnit(double value) {
    return std::max(0.0, std::min(1.0, value));
}

double ComputeEvidenceStrength(const DetectorResult& result) {
    const double persistence_ratio =
        std::min(1.0, static_cast<double>(result.persistence) / RelationPatternPersistenceWindow());
    return ClipUnit(result.normalized_score) * ClipUnit(result.confidence) * persistence_ratio;
}

std::string CopyStringRef(const BaselineStringRef& ref) {
    if (!ref.data || ref.size == 0) return "";
    return std::string(ref.data, ref.size);
}

double GeomMean(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    double product = 1.0;
    for (double value : values) {
        if (value <= 0.0) return 0.0;
        product *= value;
    }
    return std::pow(product, 1.0 / static_cast<double>(values.size()));
}

double Top2Mean(const std::vector<double>& values) {
    std::array<double, 2> top{0.0, 0.0};
    size_t count = 0;
    for (double value : values) {
        if (value <= 0.0) continue;
        if (count < top.size()) {
            top[count++] = value;
            if (count == top.size() && top[0] < top[1]) std::swap(top[0], top[1]);
            continue;
        }
        if (value > top[0]) {
            top[1] = top[0];
            top[0] = value;
        } else if (value > top[1]) {
            top[1] = value;
        }
    }

    if (count == 0) return 0.0;
    if (count == 1) return top[0];
    return 0.5 * (top[0] + top[1]);
}

void InsertTopSingle(const StoredDominantSingleProjection& candidate,
                     std::array<StoredDominantSingleProjection, kBaselineDominantSingleLimit>* top,
                     uint32_t* count) {
    if (!top || !count || candidate.a_f <= 0.0) return;

    uint32_t insert_at = *count;
    while (insert_at > 0 && (*top)[insert_at - 1].a_f < candidate.a_f) {
        if (insert_at < kBaselineDominantSingleLimit) {
            (*top)[insert_at] = (*top)[insert_at - 1];
        }
        --insert_at;
    }

    if (insert_at >= kBaselineDominantSingleLimit) return;
    (*top)[insert_at] = candidate;
    if (*count < kBaselineDominantSingleLimit) {
        ++(*count);
    }
}

void InsertTopPattern(const StoredDominantPatternProjection& candidate,
                      std::array<StoredDominantPatternProjection, kBaselineDominantPatternLimit>* top,
                      uint32_t* count) {
    if (!top || !count || candidate.weighted_score <= 0.0) return;

    uint32_t insert_at = *count;
    while (insert_at > 0 &&
           (*top)[insert_at - 1].weighted_score < candidate.weighted_score) {
        if (insert_at < kBaselineDominantPatternLimit) {
            (*top)[insert_at] = (*top)[insert_at - 1];
        }
        --insert_at;
    }

    if (insert_at >= kBaselineDominantPatternLimit) return;
    (*top)[insert_at] = candidate;
    if (*count < kBaselineDominantPatternLimit) {
        ++(*count);
    }
}

void AppendSupportingFeature(const std::string& feature,
                             std::vector<std::string>* out_features) {
    if (!out_features || feature.empty()) return;
    if (std::find(out_features->begin(), out_features->end(), feature) != out_features->end()) {
        return;
    }
    out_features->push_back(feature);
}

const EvidenceSignal* FindSignal(const MetricEvidenceBundle& bundle,
                                 RelationSummaryKind kind) {
    auto it = bundle.by_summary.find(kind);
    if (it == bundle.by_summary.end()) return nullptr;
    return &it->second;
}

MetricPatternSignal ComputeSupportEscape(const MetricEvidenceBundle& bundle) {
    MetricPatternSignal signal;
    const EvidenceSignal* out_support = FindSignal(bundle, RelationSummaryKind::kOutOfSupportShare);
    if (!out_support || out_support->a_up <= 0.0) return signal;

    signal.available = true;
    const EvidenceSignal* entropy = FindSignal(bundle, RelationSummaryKind::kEntropyShannon);
    const EvidenceSignal* distinct =
        FindSignal(bundle, RelationSummaryKind::kDistinctGroupCount);
    const EvidenceSignal* coverage =
        FindSignal(bundle, RelationSummaryKind::kStableHeadCoverage);
    const EvidenceSignal* top1 = FindSignal(bundle, RelationSummaryKind::kTop1Share);
    const EvidenceSignal* headk = FindSignal(bundle, RelationSummaryKind::kHeadKShare);

    const double core = out_support->a_up;
    const double support = Top2Mean({
        entropy ? entropy->a_up : 0.0,
        distinct ? distinct->a_up : 0.0,
        coverage ? coverage->a_down : 0.0,
    });
    const double oppose = Top2Mean({
        top1 ? top1->a_up : 0.0,
        headk ? headk->a_up : 0.0,
        entropy ? entropy->a_down : 0.0,
    });

    signal.score =
        ClipUnit(core + RelationPatternLambdaSup() * support - RelationPatternLambdaOpp() * oppose);
    if (signal.score <= 0.0) return signal;

    AppendSupportingFeature(out_support->feature, &signal.supporting_features);
    if (entropy && entropy->a_up > 0.0) AppendSupportingFeature(entropy->feature,
                                                                &signal.supporting_features);
    if (distinct && distinct->a_up > 0.0) AppendSupportingFeature(distinct->feature,
                                                                  &signal.supporting_features);
    if (coverage && coverage->a_down > 0.0) AppendSupportingFeature(coverage->feature,
                                                                    &signal.supporting_features);
    return signal;
}

MetricPatternSignal ComputeHeadConcentration(const MetricEvidenceBundle& bundle) {
    MetricPatternSignal signal;
    const EvidenceSignal* top1 = FindSignal(bundle, RelationSummaryKind::kTop1Share);
    if (!top1 || top1->a_up <= 0.0) return signal;

    signal.available = true;
    const EvidenceSignal* headk = FindSignal(bundle, RelationSummaryKind::kHeadKShare);
    const EvidenceSignal* entropy = FindSignal(bundle, RelationSummaryKind::kEntropyShannon);
    const EvidenceSignal* out_support = FindSignal(bundle, RelationSummaryKind::kOutOfSupportShare);

    const double core = top1->a_up;
    const double support = Top2Mean({
        headk ? headk->a_up : 0.0,
        entropy ? entropy->a_down : 0.0,
    });
    const double oppose = Top2Mean({
        out_support ? out_support->a_up : 0.0,
        entropy ? entropy->a_up : 0.0,
    });

    signal.score =
        ClipUnit(core + RelationPatternLambdaSup() * support - RelationPatternLambdaOpp() * oppose);
    if (signal.score <= 0.0) return signal;

    AppendSupportingFeature(top1->feature, &signal.supporting_features);
    if (headk && headk->a_up > 0.0) AppendSupportingFeature(headk->feature,
                                                            &signal.supporting_features);
    if (entropy && entropy->a_down > 0.0) AppendSupportingFeature(entropy->feature,
                                                                  &signal.supporting_features);
    return signal;
}

MetricPatternSignal ComputeLegacyHeadDilution(const MetricEvidenceBundle& bundle) {
    MetricPatternSignal signal;
    const EvidenceSignal* coverage =
        FindSignal(bundle, RelationSummaryKind::kStableHeadCoverage);
    if (!coverage || coverage->a_down <= 0.0) return signal;

    signal.available = true;
    const EvidenceSignal* out_support = FindSignal(bundle, RelationSummaryKind::kOutOfSupportShare);
    const EvidenceSignal* entropy = FindSignal(bundle, RelationSummaryKind::kEntropyShannon);

    const double core = coverage->a_down;
    const double support = Top2Mean({
        out_support ? out_support->a_up : 0.0,
        entropy ? entropy->a_up : 0.0,
    });
    const double oppose = coverage->a_up;

    signal.score =
        ClipUnit(core + RelationPatternLambdaSup() * support - RelationPatternLambdaOpp() * oppose);
    if (signal.score <= 0.0) return signal;

    AppendSupportingFeature(coverage->feature, &signal.supporting_features);
    if (out_support && out_support->a_up > 0.0) AppendSupportingFeature(out_support->feature,
                                                                        &signal.supporting_features);
    if (entropy && entropy->a_up > 0.0) AppendSupportingFeature(entropy->feature,
                                                                &signal.supporting_features);
    return signal;
}

MetricPatternSignal ComputeStableHeadMixShift(const MetricEvidenceBundle& bundle) {
    MetricPatternSignal signal;
    const EvidenceSignal* mix_drift =
        FindSignal(bundle, RelationSummaryKind::kStableHeadMixDrift);
    if (!mix_drift || mix_drift->a_up <= 0.0) return signal;

    signal.available = true;
    const EvidenceSignal* coverage =
        FindSignal(bundle, RelationSummaryKind::kStableHeadCoverage);
    const EvidenceSignal* out_support = FindSignal(bundle, RelationSummaryKind::kOutOfSupportShare);

    const double core = mix_drift->a_up;
    const double oppose = Top2Mean({
        coverage ? coverage->a_down : 0.0,
        out_support ? out_support->a_up : 0.0,
    });

    signal.score = ClipUnit(core - RelationPatternLambdaOpp() * oppose);
    if (signal.score <= 0.0) return signal;

    AppendSupportingFeature(mix_drift->feature, &signal.supporting_features);
    return signal;
}

MetricPatternSignal ComputePatternScore(PatternCode pattern,
                                        const MetricEvidenceBundle& bundle) {
    switch (pattern) {
        case PatternCode::kSupportEscape:
            return ComputeSupportEscape(bundle);
        case PatternCode::kHeadConcentration:
            return ComputeHeadConcentration(bundle);
        case PatternCode::kLegacyHeadDilution:
            return ComputeLegacyHeadDilution(bundle);
        case PatternCode::kStableHeadMixShift:
            return ComputeStableHeadMixShift(bundle);
        case PatternCode::kUnknown:
            break;
    }
    return MetricPatternSignal{};
}

}  // namespace

const char* PatternCodeName(PatternCode code) {
    switch (code) {
        case PatternCode::kSupportEscape:
            return "support_escape";
        case PatternCode::kHeadConcentration:
            return "head_concentration";
        case PatternCode::kLegacyHeadDilution:
            return "legacy_head_dilution";
        case PatternCode::kStableHeadMixShift:
            return "stable_head_mix_shift";
        case PatternCode::kUnknown:
            break;
    }
    return "unknown";
}

PatternCode PatternCodeFromName(const std::string& name) {
    if (name == "support_escape") return PatternCode::kSupportEscape;
    if (name == "head_concentration") return PatternCode::kHeadConcentration;
    if (name == "legacy_head_dilution") return PatternCode::kLegacyHeadDilution;
    if (name == "stable_head_mix_shift") return PatternCode::kStableHeadMixShift;
    return PatternCode::kUnknown;
}

double PatternWeight(PatternCode code) {
    switch (code) {
        case PatternCode::kSupportEscape:
        case PatternCode::kHeadConcentration:
            return 0.7;
        case PatternCode::kLegacyHeadDilution:
        case PatternCode::kStableHeadMixShift:
            return 0.85;
        case PatternCode::kUnknown:
            break;
    }
    return 0.0;
}

int32_t PatternLocalSlot(PatternCode code) {
    return kRelationPatternLocalSlotBase + static_cast<int32_t>(code);
}

int RelationPatternFusion::Compute(const RelationPatternFusionInput& input,
                                   RelationPatternFusionOutput* out) {
    if (!out) return error::BAD_REQUEST;
    *out = RelationPatternFusionOutput{};

    out->fusion_result.available = true;
    out->fusion_result.key = input.key;
    out->fusion_result.ts = input.bucket_id;

    std::unordered_map<std::string, MetricEvidenceBundle> bundles;
    std::vector<std::pair<double, StoredDominantSingleProjection>> ranked_singles;
    ranked_singles.reserve(input.singles.size());

    for (const auto& single : input.singles) {
        EvidenceSignal signal;
        signal.present = true;
        signal.a_f = ComputeEvidenceStrength(single.detector_result);
        signal.a_up =
            single.detector_result.direction == BaselineDirection::kUp ? signal.a_f : 0.0;
        signal.a_down =
            single.detector_result.direction == BaselineDirection::kDown ? signal.a_f : 0.0;
        signal.feature = CopyStringRef(single.detector_result.feature);
        signal.single_projection.feature = signal.feature;
        signal.single_projection.dir = single.detector_result.direction;
        signal.single_projection.reason_code = single.detector_result.reason_code;
        signal.single_projection.a_f = signal.a_f;
        signal.single_projection.normalized_score = single.detector_result.normalized_score;
        signal.single_projection.confidence = single.detector_result.confidence;
        signal.single_projection.persistence = single.detector_result.persistence;

        auto& bundle = bundles[single.metric_name];
        bundle.metric_name = single.metric_name;
        bundle.by_summary[single.summary_kind] = signal;

        if (signal.a_f > 0.0) {
            ranked_singles.emplace_back(signal.a_f, signal.single_projection);
        }
    }

    std::sort(ranked_singles.begin(), ranked_singles.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
    for (const auto& item : ranked_singles) {
        InsertTopSingle(item.second,
                        &out->fusion_result.dominant_singles,
                        &out->fusion_result.dominant_single_count);
    }

    std::vector<double> single_evidence;
    single_evidence.reserve(ranked_singles.size());
    for (const auto& item : ranked_singles) single_evidence.push_back(item.first);
    double risk_single = 0.0;
    if (!single_evidence.empty()) {
        double product = 1.0;
        for (double value : single_evidence) product *= (1.0 - value);
        risk_single = 1.0 - product;
    }

    std::vector<std::pair<double, StoredDominantPatternProjection>> ranked_patterns;
    for (PatternCode pattern : {PatternCode::kSupportEscape,
                                PatternCode::kHeadConcentration,
                                PatternCode::kLegacyHeadDilution,
                                PatternCode::kStableHeadMixShift}) {
        double score_pattern = 0.0;
        std::vector<std::pair<double, std::string>> metric_hits;
        std::vector<std::string> supporting_features;
        double product = 1.0;

        for (const auto& entry : bundles) {
            const MetricPatternSignal local = ComputePatternScore(pattern, entry.second);
            if (!local.available || local.score <= 0.0) continue;

            product *= (1.0 - local.score);
            metric_hits.emplace_back(local.score, entry.first);
            for (const auto& feature : local.supporting_features) {
                AppendSupportingFeature(feature, &supporting_features);
            }
        }

        if (metric_hits.empty()) continue;

        score_pattern = 1.0 - product;
        StoredDominantPatternProjection projection;
        projection.pattern = PatternCodeName(pattern);
        projection.feature_base = input.feature_base;
        projection.score_pattern = score_pattern;
        projection.weighted_score = PatternWeight(pattern) * score_pattern;

        std::sort(metric_hits.begin(), metric_hits.end(),
                  [](const auto& lhs, const auto& rhs) {
                      return lhs.first > rhs.first;
                  });
        for (size_t i = 0; i < metric_hits.size() &&
                           i < static_cast<size_t>(kBaselinePatternMetricsHitLimit);
             ++i) {
            AppendStoredMetricHit(&projection, metric_hits[i].second);
        }
        for (size_t i = 0; i < supporting_features.size() &&
                           i < static_cast<size_t>(kBaselinePatternSupportingFeatureLimit);
             ++i) {
            AppendStoredSupportingFeature(&projection, supporting_features[i]);
        }

        ranked_patterns.emplace_back(projection.weighted_score, projection);

        FusionPatternContribution contribution;
        contribution.pattern = pattern;
        contribution.local_slot = PatternLocalSlot(pattern);
        contribution.projection = projection;
        out->pattern_contributions.push_back(std::move(contribution));
    }

    std::sort(ranked_patterns.begin(), ranked_patterns.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
    for (const auto& item : ranked_patterns) {
        InsertTopPattern(item.second,
                         &out->fusion_result.dominant_patterns,
                         &out->fusion_result.dominant_pattern_count);
    }

    double risk_pattern = 0.0;
    if (!ranked_patterns.empty()) {
        double product = 1.0;
        for (const auto& item : ranked_patterns) {
            product *= (1.0 - item.first);
        }
        risk_pattern = 1.0 - product;
    }

    out->fusion_result.risk =
        1.0 - (1.0 - risk_single) * (1.0 - risk_pattern);
    return error::OK;
}

}  // namespace baseline
}  // namespace flowsql
