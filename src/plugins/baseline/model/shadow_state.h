/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_MODEL_SHADOW_STATE_H_
#define _FLOWSQL_PLUGINS_BASELINE_MODEL_SHADOW_STATE_H_

#include <cstdint>
#include <memory>
#include <cstddef>
#include <string>
#include <vector>

#include "formal_model.h"

namespace flowsql {
namespace baseline {

enum class ShadowRefKind : uint8_t {
    kNone = 0,
    kSelfFormal = 1,
    kSelfCandidate = 2,
    kSourceFormal = 3,
    kSourceCandidate = 4,
};

inline const char* ShadowRefKindName(ShadowRefKind kind) {
    switch (kind) {
        case ShadowRefKind::kSelfFormal:
            return "self_formal";
        case ShadowRefKind::kSelfCandidate:
            return "self_candidate";
        case ShadowRefKind::kSourceFormal:
            return "source_formal";
        case ShadowRefKind::kSourceCandidate:
            return "source_candidate";
        case ShadowRefKind::kNone:
            break;
    }
    return "none";
}

inline bool ShadowRefUsesSource(ShadowRefKind kind) {
    return kind == ShadowRefKind::kSourceFormal || kind == ShadowRefKind::kSourceCandidate;
}

template <typename TFormalModel>
struct ShadowStateT {
    bool active = false;
    ShadowRefKind ref_kind = ShadowRefKind::kNone;
    std::string ref_source_key;
    uint64_t ref_model_version = 0;
    std::shared_ptr<TFormalModel> frozen_ref_model;
    double delta = 0.0;
    int64_t last_bucket_id = 0;
    int64_t enter_bucket_id = -1;
    uint64_t shadow_point_count = 0;
    uint64_t shadow_effective_holdout_count = 0;
    int64_t last_candidate_attempt_bucket = -1;
    bool shadow_stuck = false;
    std::vector<double> slow_err_ring;
    std::vector<double> slow_var_ring;
    std::size_t slow_ring_pos = 0;
    std::size_t slow_ring_size = 0;
    double slow_err_sum = 0.0;
    double slow_var_sum = 0.0;
    double z_win = 0.0;
    bool slow_drift_triggered = false;

    void Reset() {
        active = false;
        ref_kind = ShadowRefKind::kNone;
        ref_source_key.clear();
        ref_model_version = 0;
        frozen_ref_model.reset();
        delta = 0.0;
        last_bucket_id = 0;
        enter_bucket_id = -1;
        shadow_point_count = 0;
        shadow_effective_holdout_count = 0;
        last_candidate_attempt_bucket = -1;
        shadow_stuck = false;
        slow_err_ring.clear();
        slow_var_ring.clear();
        slow_ring_pos = 0;
        slow_ring_size = 0;
        slow_err_sum = 0.0;
        slow_var_sum = 0.0;
        z_win = 0.0;
        slow_drift_triggered = false;
    }
};

using ValueShadowState = ShadowStateT<ValueFormalModel>;
using RatioShadowState = ShadowStateT<RatioFormalModel>;

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_MODEL_SHADOW_STATE_H_
