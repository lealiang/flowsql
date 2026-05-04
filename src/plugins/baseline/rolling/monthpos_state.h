/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_ROLLING_MONTHPOS_STATE_H_
#define _FLOWSQL_PLUGINS_BASELINE_ROLLING_MONTHPOS_STATE_H_

#include <framework/interfaces/ibaseline_types.h>

#include "plugins/baseline/bootstrap/bootstrap_types.h"
#include "plugins/baseline/model/calendar_feature_helper.h"
#include "plugins/baseline/rolling/observation_adapter.h"
#include "plugins/baseline/rolling/rolling_state.h"

namespace flowsql {
namespace baseline {

BaselineStatus InitializeRollingMonthposFromSeed(const BootstrapSeed& seed,
                                                 const BaselineRollingConfig& config,
                                                 RollingState* state);

double EvaluateRollingMonthpos(const RollingState& state,
                               int64_t bucket_id,
                               const BaselineRollingConfig& config);

double EvaluateRollingMonthposWithFeature(const RollingState& state,
                                          const LocalCalendarFeature& feature);

BaselineStatus UpdateRollingMonthpos(const ObservedModelPoint& point,
                                     double monthpos_residual,
                                     double update_weight,
                                     const BaselineRollingConfig& config,
                                     RollingState* state);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_ROLLING_MONTHPOS_STATE_H_
