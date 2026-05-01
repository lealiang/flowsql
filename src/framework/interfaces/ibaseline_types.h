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
#include <string>
#include <utility>
#include <vector>

namespace flowsql {

enum class BaselineSerializationFormat : int32_t {
    kJson = 0,
};

enum class BaselineStatus : int32_t {
    kOk = 0,
    kNotTrained = 1,
    kInsufficientData = 2,
    kInvalidArgument = 3,
    kCalendarUnavailable = 4,
    kIncompatibleArtifact = 5,
    kUnsupportedFormat = 6,
    kParseFailed = 7,
    kTrainFailed = 8,
    kPredictFailed = 9,
    kSerializationFailed = 10,
};

enum class BootstrapSeedStatus : int32_t {
    kNone = 0,
    kWeak = 1,
    kPartial = 2,
    kFull = 3,
};

using BaselineSerializedContent = std::string;
using BaselineSerializationResult =
    std::pair<BaselineStatus, BaselineSerializedContent>;

enum class BaselineTaskKind : int32_t {
    kValue = 0,
    kRatio = 1,
    kRelation = 2,
};

struct BootstrapTrainOptions {
    bool force_replace_existing_artifact = true;
    bool enable_monthpos = true;
    bool enable_event = true;
    bool include_diagnostics = true;
    uint32_t min_observation_count = 0;
};

struct ValueBootstrapPoint {
    int64_t bucket_id = 0;
    double value = 0.0;
    uint64_t sample_count = 0;
};

struct RatioBootstrapPoint {
    int64_t bucket_id = 0;
    double numerator = 0.0;
    double denominator = 0.0;
};

struct RelationBootstrapMetric {
    std::string metric;
    double total = 0.0;
    uint32_t active_count = 0;
    std::vector<double> values_by_group;
};

struct RelationBootstrapBlock {
    int64_t bucket_id = 0;
    std::vector<uint32_t> group_idx;
    std::vector<RelationBootstrapMetric> metrics;
};

struct ValueBootstrapInput {
    std::string series_key;
    std::vector<ValueBootstrapPoint> observations;
    BootstrapTrainOptions options;
};

struct RatioBootstrapInput {
    std::string series_key;
    std::vector<RatioBootstrapPoint> observations;
    BootstrapTrainOptions options;
};

struct RelationBootstrapInput {
    std::string series_key;
    std::vector<RelationBootstrapBlock> blocks;
    BootstrapTrainOptions options;
};

struct BootstrapTrainResult {
    BaselineStatus status = BaselineStatus::kOk;
    BootstrapSeedStatus seed_status = BootstrapSeedStatus::kNone;
    uint64_t accepted_count = 0;
    uint64_t rejected_count = 0;
    int64_t train_start_bucket = 0;
    int64_t train_end_bucket = 0;
    double coverage_ratio = 0.0;
    double confidence = 0.0;
    std::vector<std::string> enabled_components;
    std::string diagnostics;

    bool ok() const { return status == BaselineStatus::kOk; }
};

struct BootstrapPredictionOptions {
    double confidence_level = 0.95;
    bool include_model_space_debug = false;
    bool include_diagnostics = false;
};

struct BootstrapPrediction {
    BaselineStatus status = BaselineStatus::kOk;
    std::string series_key;
    int64_t bucket_id = 0;
    double baseline_mu = 0.0;
    double baseline_lower = 0.0;
    double baseline_upper = 0.0;
    double band_width = 0.0;
    double confidence = 0.0;
    std::vector<std::string> uncertainty_source;

    bool has_model_space = false;
    double model_space_mu = 0.0;
    double model_space_lower = 0.0;
    double model_space_upper = 0.0;

    std::string diagnostics;

    bool ok() const { return status == BaselineStatus::kOk; }
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBASELINE_TYPES_H_
