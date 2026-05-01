/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_MODEL_FORMAL_MODEL_H_
#define _FLOWSQL_PLUGINS_BASELINE_MODEL_FORMAL_MODEL_H_

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "plugins/baseline/model/readiness_helper.h"

namespace flowsql {
namespace baseline {

enum class FormalModelKind : int32_t {
    kNone = 0,
    kValueBaseline = 1,
    kRatioBaseline = 2,
};

const char* FormalModelKindName(FormalModelKind kind);

enum class TransformKind : int32_t {
    kIdentity = 0,
    kLog1p = 1,
    kLogit = 2,
};

const char* TransformKindName(TransformKind kind);
TransformKind ParseTransformKind(const std::string& name);

struct FormalModelMetadata {
    FormalModelKind kind = FormalModelKind::kNone;
    uint64_t model_version = 0;
    int64_t train_bucket_start = 0;
    int64_t train_bucket_end = 0;
    uint64_t holdout_count = 0;
    uint64_t observation_count = 0;
    std::string calendar_id;
    std::string calendar_version;
};

struct CoreBlock {
    double beta0 = 0.0;
    double trend_k = 0.0;
    std::vector<double> day_sin;
    std::vector<double> day_cos;
    std::vector<double> week_sin;
    std::vector<double> week_cos;
};

struct MonthPosBlock {
    bool enabled = false;
    std::array<double, 31> dom_coeff{};
    std::vector<double> dme_coeff;
    std::array<double, 7> lwd_coeff{};
    std::array<double, 31> dom_center{};
    std::vector<double> dme_center;
    std::array<double, 7> lwd_center{};
};

struct EventBlock {
    bool enabled = false;
    std::string calendar_id;
    std::string calendar_version;
    std::vector<std::string> active_event_codes;
    std::vector<double> coeff;
};

struct FitBlockDigest {
    std::string block_name;
    std::string status;
    uint64_t sample_count = 0;
    double objective = 0.0;
    double condition_est = 0.0;
};

struct ValueFormalModel {
    FormalModelMetadata metadata;
    ModelReadiness readiness = ModelReadiness::kNotReady;
    std::string transform_name = "log1p";
    std::string solver_name = "weighted_huber_ridge_irls";
    std::string fit_strategy = "stage_fit";
    int64_t delta = 0;
    std::string tz;
    std::string profile;
    CoreBlock core_block;
    MonthPosBlock monthpos_block;
    EventBlock event_block;
    std::vector<FitBlockDigest> fit_summary;
    double sigma_ref = 0.0;
    int64_t train_start = 0;
    int64_t train_end = 0;
    double confidence_base_at_train = 0.0;
};

struct RatioFormalModel {
    FormalModelMetadata metadata;
    ModelReadiness readiness = ModelReadiness::kNotReady;
    std::string transform_name = "logit";
    std::string solver_name = "weighted_huber_ridge_irls";
    std::string fit_strategy = "stage_fit";
    double m0 = 0.5;
    double alpha0 = 0.0;
    double beta0 = 0.0;
    int64_t delta = 0;
    std::string tz;
    std::string profile;
    CoreBlock core_block;
    MonthPosBlock monthpos_block;
    EventBlock event_block;
    std::vector<FitBlockDigest> fit_summary;
    double sigma_ref = 0.0;
    int64_t train_start = 0;
    int64_t train_end = 0;
    double confidence_base_at_train = 0.0;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_MODEL_FORMAL_MODEL_H_
