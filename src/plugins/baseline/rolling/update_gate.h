/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_ROLLING_UPDATE_GATE_H_
#define _FLOWSQL_PLUGINS_BASELINE_ROLLING_UPDATE_GATE_H_

#include "plugins/baseline/rolling/rolling_config.h"

namespace flowsql {
namespace baseline {

struct UpdateGateResult {
    double skip_threshold = 0.0;
    double gate_update_weight = 0.0;
    bool skip_update = false;
    bool downweight_update = false;
};

UpdateGateResult ComputeUpdateGate(double z_score,
                                   double adapt_boost,
                                   const BaselineRollingConfig& config);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_ROLLING_UPDATE_GATE_H_
