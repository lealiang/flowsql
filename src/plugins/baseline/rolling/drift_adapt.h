/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_ROLLING_DRIFT_ADAPT_H_
#define _FLOWSQL_PLUGINS_BASELINE_ROLLING_DRIFT_ADAPT_H_

#include <framework/interfaces/ibaseline_types.h>

#include "plugins/baseline/rolling/rolling_state.h"

namespace flowsql {
namespace baseline {

struct DriftAdaptResult {
    double resid_norm = 0.0;
    double drift_evidence = 0.0;
    double level_shift_evidence = 0.0;
    double combined_drift_evidence = 0.0;
    double adapt_boost = 0.0;
};

BaselineStatus UpdateDriftEvidence(double residual,
                                   double band_std,
                                   bool can_score,
                                   const BaselineRollingConfig& config,
                                   RollingState* state,
                                   DriftAdaptResult* out);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_ROLLING_DRIFT_ADAPT_H_
