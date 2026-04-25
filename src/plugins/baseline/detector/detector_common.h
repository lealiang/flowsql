/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_DETECTOR_DETECTOR_COMMON_H_
#define _FLOWSQL_PLUGINS_BASELINE_DETECTOR_DETECTOR_COMMON_H_

#include <framework/interfaces/ibaseline_types.h>

#include <string>

#include "plugins/baseline/model/formal_model_state.h"

namespace flowsql {
namespace baseline {

struct RebuildIntent {
    bool required = false;
    BaselineRebuildReason reason = BaselineRebuildReason::kManual;
    int64_t rebuild_start_hint = 0;
    int64_t bucket_end = 0;
    std::string routed_feature_id;
};

struct DetectorSubmitOutput {
    DetectorResult detector_result;
    RebuildIntent rebuild_intent;
};

struct DetectorRebuildFailure {
    std::string key;
    int64_t request_bucket_start = 0;
    int64_t request_bucket_end = 0;
    RebuildCandidateState candidate_state = RebuildCandidateState::kFailed;
    RebuildSwitchState switch_state = RebuildSwitchState::kIdle;
    RebuildFailureReason failure_reason = RebuildFailureReason::kUnavailable;
    std::string failure_reason_detail;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_DETECTOR_DETECTOR_COMMON_H_
