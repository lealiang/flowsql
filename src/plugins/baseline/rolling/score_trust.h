/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_ROLLING_SCORE_TRUST_H_
#define _FLOWSQL_PLUGINS_BASELINE_ROLLING_SCORE_TRUST_H_

#include <framework/interfaces/ibaseline_types.h>

#include <string>

#include "plugins/baseline/rolling/observation_adapter.h"
#include "plugins/baseline/rolling/rolling_state.h"

namespace flowsql {
namespace baseline {

struct ScoreTrustResult {
    BaselineStatus status = BaselineStatus::kOk;
    bool can_alert = false;
    double score_confidence = 0.0;
    double effective_confidence = 0.0;
    std::string reason;
};

BaselineStatus UpdateScoreTrust(const ObservedModelPoint& point,
                                double detection_z,
                                const BaselineRollingConfig& config,
                                RollingState* state,
                                ScoreTrustResult* out);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_ROLLING_SCORE_TRUST_H_
