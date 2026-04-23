/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_MODEL_DRIFT_STATE_H_
#define _FLOWSQL_PLUGINS_BASELINE_MODEL_DRIFT_STATE_H_

#include <algorithm>
#include <cstdint>

namespace flowsql {
namespace baseline {

enum class DriftDirection : uint8_t {
    kNone = 0,
    kUp = 1,
    kDown = 2,
};

inline const char* DriftDirectionName(DriftDirection direction) {
    switch (direction) {
        case DriftDirection::kUp:
            return "up";
        case DriftDirection::kDown:
            return "down";
        case DriftDirection::kNone:
            break;
    }
    return "none";
}

struct DriftConfig {
    double alpha = 0.2;
    double shift_clip = 6.0;
    double lambda_mem = 0.9;
    double kappa_shift = 0.25;
    double u_min = 0.5;
    double h_shift = 3.0;
    double p_shift_low = 0.3;
    double p_shift_high = 0.6;
    uint32_t m_shift = 3;
    uint32_t g_skip = 3;
    uint32_t g_reset = 12;
};

struct DriftUpdateResult {
    uint32_t gap = 0;
    double p_shift = 0.0;
    bool shift_confirmed = false;
    bool reset_before_update = false;
    DriftDirection direction = DriftDirection::kNone;
};

// 这里实现的是设计文档里的“漂移证据累积器（BOCPD-style）”最小在线状态，
// 只保留 O(1) 状态来判断“旧基线是否持续失配”，不实现完整 BOCPD 后验。
struct DriftState {
    double evidence_up = 0.0;
    double evidence_down = 0.0;
    double smoothed_residual = 0.0;
    uint32_t confirm_count = 0;
    uint32_t low_count = 0;
    DriftDirection direction = DriftDirection::kNone;
    int64_t last_bucket_id = 0;
    bool initialized = false;

    void Reset() {
        evidence_up = 0.0;
        evidence_down = 0.0;
        smoothed_residual = 0.0;
        confirm_count = 0;
        low_count = 0;
        direction = DriftDirection::kNone;
        last_bucket_id = 0;
        initialized = false;
    }
};

inline double ClipDriftValue(double value, double lower, double upper) {
    return std::max(lower, std::min(upper, value));
}

inline double DriftProbability(const DriftState& state, const DriftConfig& config) {
    if (config.h_shift <= 0.0) return 0.0;
    return ClipDriftValue(std::max(state.evidence_up, state.evidence_down) / config.h_shift, 0.0, 1.0);
}

inline DriftUpdateResult UpdateDriftState(DriftState* state,
                                          const DriftConfig& config,
                                          int64_t bucket_id,
                                          double signed_residual,
                                          bool gate_shift) {
    DriftUpdateResult result;
    if (!state) return result;

    if (state->initialized && bucket_id > state->last_bucket_id + 1) {
        result.gap = static_cast<uint32_t>(bucket_id - state->last_bucket_id - 1);
        if (result.gap > config.g_reset) {
            state->Reset();
            result.reset_before_update = true;
        }
    }

    if (gate_shift) {
        const double clipped =
            ClipDriftValue(signed_residual, -config.shift_clip, config.shift_clip);
        state->smoothed_residual =
            config.alpha * clipped + (1.0 - config.alpha) * state->smoothed_residual;

        state->evidence_up = std::max(
            0.0, config.lambda_mem * state->evidence_up + state->smoothed_residual - config.kappa_shift);
        state->evidence_down = std::max(
            0.0, config.lambda_mem * state->evidence_down - state->smoothed_residual - config.kappa_shift);

        if (state->evidence_up > state->evidence_down) {
            state->direction = DriftDirection::kUp;
        } else if (state->evidence_down > state->evidence_up) {
            state->direction = DriftDirection::kDown;
        } else {
            state->direction = DriftDirection::kNone;
        }

        if ((state->direction == DriftDirection::kUp && state->smoothed_residual >= config.u_min) ||
            (state->direction == DriftDirection::kDown &&
             state->smoothed_residual <= -config.u_min)) {
            ++state->confirm_count;
        } else if (state->confirm_count > 0) {
            --state->confirm_count;
        }

        result.p_shift = DriftProbability(*state, config);
        if (result.p_shift >= config.p_shift_low) {
            ++state->low_count;
        } else {
            state->low_count = 0;
        }
        result.shift_confirmed =
            result.p_shift >= config.p_shift_high || state->low_count >= config.m_shift;
    } else {
        result.p_shift = DriftProbability(*state, config);
    }

    state->last_bucket_id = bucket_id;
    state->initialized = true;
    result.direction = state->direction;
    return result;
}

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_MODEL_DRIFT_STATE_H_
