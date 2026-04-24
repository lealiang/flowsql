/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IBASELINE_TYPES_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IBASELINE_TYPES_H_

#include <common/typedef.h>

#include <cstdint>

namespace flowsql {

struct BaselineStringRef {
    const char* data = nullptr;
    uint32_t size = 0;
};

enum class BaselineTaskKind : int32_t {
    kValue = 0,
    kRatio = 1,
    kRelation = 2,
};

enum class BaselineDirection : int32_t {
    kUnknown = 0,
    kUp = 1,
    kDown = 2,
};

enum class BaselineSeverity : int32_t {
    kInfo = 0,
    kLow = 1,
    kMedium = 2,
    kHigh = 3,
};

enum class BaselineProvider : int32_t {
    kFormal = 0,
    kShadow = 1,
    kSource = 2,
    kNone = 3,
};

enum class BaselineSourceKind : int32_t {
    kSelf = 0,
    kConfiguredSource = 1,
    kNone = 2,
};

enum class BaselineEvidenceKind : int32_t {
    kNone = 0,
    kValue = 1,
    kRatio = 2,
};

enum class BaselineModelState : int32_t {
    kUnknown = 0,
    kColdStart = 1,
    kFormal = 2,
    kShadow = 3,
    kConfiguredSource = 4,
    kCandidate = 5,
};

enum class BaselineReasonCode : int32_t {
    kUnknown = 0,
    kSpike = 1,
    kDrop = 2,
    kBaselineShiftUp = 3,
    kBaselineShiftDown = 4,
    kDrift = 5,
    kScan = 6,
    kRarePeer = 7,
};

enum class BaselineRebuildReason : int32_t {
    kManual = 0,
    kShiftConfirmed = 1,
    kScheduled = 2,
    kBootstrap = 3,
};

enum BaselineResultFlag : uint64_t {
    kBaselineFlagNone = 0,
    kBaselineFlagColdStart = 1ULL << 0,
    kBaselineFlagGapBefore = 1ULL << 1,
    kBaselineFlagOutOfOrder = 1ULL << 2,
    kBaselineFlagRebuildQueued = 1ULL << 3,
    kBaselineFlagShadowActive = 1ULL << 4,
};

enum RelationMetricFlag : uint32_t {
    kRelationMetricFlagNone = 0,
    kRelationMetricHasActiveCount = 1U << 0,
};

enum BaselineEvidenceFieldFlag : uint32_t {
    kBaselineEvidenceFieldNone = 0,
    kBaselineEvidenceHasSampleCount = 1U << 0,
    kBaselineEvidenceHasSigmaEff = 1U << 1,
    kBaselineEvidenceHasSourceKey = 1U << 2,
};

struct ValueEvidence {
    uint32_t field_flags = 0;
    double y_t = 0.0;
    double x_t = 0.0;
    double baseline_mu_t = 0.0;
    double resid_r_t = 0.0;
    double z_t = 0.0;
    double p_shift_t = 0.0;
    double dir_t = 0.0;
    double score_point = 0.0;
    double score_shift = 0.0;
    uint64_t sample_count = 0;
    double sigma_eff_t = 0.0;
    BaselineSourceKind baseline_source_kind = BaselineSourceKind::kNone;
    BaselineStringRef baseline_source_key;
    BaselineModelState model_state = BaselineModelState::kUnknown;
    bool shadow_active = false;
};

struct RatioEvidence {
    uint32_t field_flags = 0;
    double numerator = 0.0;
    double denominator = 0.0;
    double p_smooth = 0.0;
    double x_t = 0.0;
    double p_hat_t = 0.0;
    double var_eff_t = 0.0;
    double r_t = 0.0;
    double rho_t = 0.0;
    double p_shift_t = 0.0;
    double dir_t = 0.0;
    double score_point = 0.0;
    double score_shift = 0.0;
    BaselineSourceKind baseline_source_kind = BaselineSourceKind::kNone;
    BaselineStringRef baseline_source_key;
    BaselineModelState model_state = BaselineModelState::kUnknown;
    bool shadow_active = false;
};

struct DetectorEvidence {
    BaselineEvidenceKind kind = BaselineEvidenceKind::kNone;
    union {
        ValueEvidence value;
        RatioEvidence ratio;
    };

    DetectorEvidence() : kind(BaselineEvidenceKind::kNone), value() {}
};

struct DetectorResult {
    int32_t status = 0;
    BaselineStringRef key;
    BaselineStringRef feature;
    BaselineStringRef feature_type;
    int64_t ts = 0;
    double raw_score = 0.0;
    double normalized_score = 0.0;
    double confidence = 0.0;
    uint32_t persistence = 0;
    BaselineDirection direction = BaselineDirection::kUnknown;
    BaselineSeverity severity = BaselineSeverity::kInfo;
    BaselineProvider provider = BaselineProvider::kFormal;
    union {
        BaselineReasonCode reason = BaselineReasonCode::kUnknown;
        BaselineReasonCode reason_code;
    };
    uint64_t flags = 0;
    DetectorEvidence evidence;
};

struct ValueObservation {
    BaselineStringRef key;
    int64_t bucket_id = 0;
    double value = 0.0;
    uint64_t sample_count = 0;
};

struct RatioObservation {
    BaselineStringRef key;
    int64_t bucket_id = 0;
    double numerator = 0.0;
    double denominator = 0.0;
};

struct RelationMetricBlock {
    double total = 0.0;
    uint32_t flags = 0;
    uint32_t active_count = 0;
    const double* values = nullptr;
};

struct RelationObservationBlock {
    BaselineStringRef key;
    int64_t bucket_id = 0;
    uint32_t nnz = 0;
    const uint32_t* group_idx = nullptr;
    uint32_t metric_count = 0;
    const RelationMetricBlock* metrics = nullptr;
};

struct HistoryFetchRequest {
    BaselineStringRef key;
    BaselineStringRef feature;
    int64_t bucket_start = 0;
    int64_t bucket_end = 0;
};

constexpr uint32_t kBaselineDominantSingleLimit = 3;
constexpr uint32_t kBaselineDominantPatternLimit = 2;
constexpr uint32_t kBaselinePatternMetricsHitLimit = 3;
constexpr uint32_t kBaselinePatternSupportingFeatureLimit = 4;

struct DominantSingleProjection {
    BaselineStringRef feature;
    BaselineDirection dir = BaselineDirection::kUnknown;
    BaselineReasonCode reason_code = BaselineReasonCode::kUnknown;
    double a_f = 0.0;
    double normalized_score = 0.0;
    double confidence = 0.0;
    uint32_t persistence = 0;
};

struct DominantPatternProjection {
    BaselineStringRef pattern;
    BaselineStringRef feature_base;
    double score_pattern = 0.0;
    uint32_t metrics_hit_count = 0;
    BaselineStringRef metrics_hit[kBaselinePatternMetricsHitLimit];
    uint32_t supporting_feature_count = 0;
    BaselineStringRef supporting_features[kBaselinePatternSupportingFeatureLimit];
};

struct FusionResult {
    BaselineStringRef key;
    int64_t ts = 0;
    double risk = 0.0;
    uint32_t dominant_single_count = 0;
    DominantSingleProjection dominant_single[kBaselineDominantSingleLimit];
    uint32_t dominant_pattern_count = 0;
    DominantPatternProjection dominant_pattern[kBaselineDominantPatternLimit];
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBASELINE_TYPES_H_
