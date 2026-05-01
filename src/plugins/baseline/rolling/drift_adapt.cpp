/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/rolling/drift_adapt.h"

#include <algorithm>
#include <cmath>

namespace flowsql {
namespace baseline {
namespace {

double Clamp(double value, double lo, double hi) {
    return std::max(lo, std::min(hi, value));
}

}  // namespace

BaselineStatus UpdateDriftEvidence(double residual,
                                   double band_std,
                                   bool can_score,
                                   const BaselineRollingConfig& config,
                                   RollingState* state,
                                   DriftAdaptResult* out) {
    if (!state || !out) return BaselineStatus::kInvalidArgument;

    DriftAdaptResult result;
    if (!can_score) {
        *out = result;
        return BaselineStatus::kOk;
    }
    if (!std::isfinite(residual) || !std::isfinite(band_std) || band_std <= 0.0) {
        return BaselineStatus::kInvalidArgument;
    }

    result.resid_norm = Clamp(residual / band_std, -config.z_cap, config.z_cap);
    state->short_ewma =
        (1.0 - config.alpha_short) * state->short_ewma + config.alpha_short * result.resid_norm;
    state->long_ewma =
        (1.0 - config.alpha_long) * state->long_ewma + config.alpha_long * result.resid_norm;
    state->drift_evidence = state->short_ewma - state->long_ewma;

    result.drift_evidence = state->drift_evidence;
    const double denominator = std::max(config.drift_full - config.drift_start, 1.0e-12);
    result.adapt_boost =
        Clamp((std::fabs(state->drift_evidence) - config.drift_start) / denominator, 0.0, 1.0);
    *out = result;
    return BaselineStatus::kOk;
}

}  // namespace baseline
}  // namespace flowsql
