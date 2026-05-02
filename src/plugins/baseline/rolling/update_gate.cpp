/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "plugins/baseline/rolling/update_gate.h"

#include <algorithm>
#include <cmath>

namespace flowsql {
namespace baseline {

UpdateGateResult ComputeUpdateGate(double z_score,
                                   double adapt_boost,
                                   const BaselineRollingConfig& config) {
    const double safe_z = std::fabs(std::isfinite(z_score) ? z_score : config.z_skip);
    const double safe_boost = std::max(0.0, std::min(1.0, adapt_boost));

    UpdateGateResult result;
    result.skip_threshold = config.z_skip + safe_boost * config.skip_relax;
    if (safe_z >= result.skip_threshold) {
        if (safe_boost >= 1.0) {
            result.downweight_update = true;
            result.gate_update_weight = config.small_update_weight;
            return result;
        }
        result.skip_update = true;
        result.gate_update_weight = 0.0;
        return result;
    }
    if (safe_z >= config.z_downweight) {
        result.downweight_update = true;
        result.gate_update_weight = config.small_update_weight;
        return result;
    }
    result.gate_update_weight = 1.0;
    return result;
}

}  // namespace baseline
}  // namespace flowsql
