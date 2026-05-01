/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_ROLLING_RESIDUAL_SCALE_H_
#define _FLOWSQL_PLUGINS_BASELINE_ROLLING_RESIDUAL_SCALE_H_

#include <framework/interfaces/ibaseline_types.h>

#include "plugins/baseline/rolling/rolling_state.h"

namespace flowsql {
namespace baseline {

struct ResidualScaleResult {
    double alpha_eff = 0.0;
    double clipped_resid2 = 0.0;
    double sigma_before = 0.0;
    double sigma_after = 0.0;
};

BaselineStatus UpdateResidualScale(double residual,
                                   double base_update_weight,
                                   const BaselineRollingConfig& config,
                                   RollingState* state,
                                   ResidualScaleResult* out);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_ROLLING_RESIDUAL_SCALE_H_
