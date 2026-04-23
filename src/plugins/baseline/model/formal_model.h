/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_MODEL_FORMAL_MODEL_H_
#define _FLOWSQL_PLUGINS_BASELINE_MODEL_FORMAL_MODEL_H_

#include <cstdint>
#include <string>

namespace flowsql {
namespace baseline {

enum class FormalModelKind : int32_t {
    kNone = 0,
    kValueInterceptFit = 1,
    kRatioInterceptFit = 2,
};

const char* FormalModelKindName(FormalModelKind kind);

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

struct ValueFormalModel {
    FormalModelMetadata metadata;
    double intercept_x = 0.0;
    double sigma_ref = 0.0;
};

struct RatioFormalModel {
    FormalModelMetadata metadata;
    double intercept_ratio = 0.0;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_MODEL_FORMAL_MODEL_H_
