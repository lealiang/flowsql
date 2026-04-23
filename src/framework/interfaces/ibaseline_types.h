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

struct DetectorResult {
    int32_t status = 0;
    double raw_score = 0.0;
    double normalized_score = 0.0;
    double confidence = 0.0;
    uint32_t persistence = 0;
    BaselineDirection direction = BaselineDirection::kUnknown;
    BaselineSeverity severity = BaselineSeverity::kInfo;
    BaselineProvider provider = BaselineProvider::kFormal;
    BaselineReasonCode reason = BaselineReasonCode::kUnknown;
    uint64_t flags = 0;
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
    int64_t bucket_start = 0;
    int64_t bucket_end = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBASELINE_TYPES_H_
