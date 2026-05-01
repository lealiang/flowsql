/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/rolling/residual_scale.h"

#include <algorithm>
#include <cmath>

namespace flowsql {
namespace baseline {

BaselineStatus UpdateResidualScale(double residual,
                                   double base_update_weight,
                                   const BaselineRollingConfig& config,
                                   RollingState* state,
                                   ResidualScaleResult* out) {
    if (!state || !out || !std::isfinite(residual) || !std::isfinite(state->sigma) ||
        state->sigma <= 0.0) {
        return BaselineStatus::kInvalidArgument;
    }

    ResidualScaleResult result;
    result.sigma_before = state->sigma;
    result.alpha_eff = config.alpha_sigma * std::max(0.0, std::min(1.0, base_update_weight));
    const double clip_cap = config.c_sigma * state->sigma;
    result.clipped_resid2 = std::min(residual * residual, clip_cap * clip_cap);
    if (result.alpha_eff > 0.0) {
        const double sigma2 = state->sigma * state->sigma;
        const double next_sigma2 =
            (1.0 - result.alpha_eff) * sigma2 + result.alpha_eff * result.clipped_resid2;
        state->sigma = std::max(config.sigma_floor, std::sqrt(std::max(0.0, next_sigma2)));
    }
    result.sigma_after = state->sigma;
    *out = result;
    return BaselineStatus::kOk;
}

}  // namespace baseline
}  // namespace flowsql
