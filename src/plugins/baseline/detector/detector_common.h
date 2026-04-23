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
    std::string candidate_state;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_DETECTOR_DETECTOR_COMMON_H_
