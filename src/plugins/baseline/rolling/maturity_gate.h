/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_ROLLING_MATURITY_GATE_H_
#define _FLOWSQL_PLUGINS_BASELINE_ROLLING_MATURITY_GATE_H_

#include <framework/interfaces/ibaseline_types.h>

#include <string>
#include <vector>

#include "plugins/baseline/rolling/observation_adapter.h"
#include "plugins/baseline/rolling/rolling_state.h"

namespace flowsql {
namespace baseline {

BaselineStatus UpdateMaturityEvidence(const ObservedModelPoint& point,
                                      const BaselineRollingConfig& config,
                                      RollingState* state);

std::vector<std::string> BuildEnabledComponents(const RollingState& state);
std::vector<std::string> BuildComponentReadiness(const RollingState& state);

double DailyCoverageRatio(const RollingState& state);
double WeeklyCoverageRatio(const RollingState& state);
double MonthposCoverageRatio(const RollingState& state);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_ROLLING_MATURITY_GATE_H_
