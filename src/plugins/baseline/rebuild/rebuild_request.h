/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_REBUILD_REBUILD_REQUEST_H_
#define _FLOWSQL_PLUGINS_BASELINE_REBUILD_REBUILD_REQUEST_H_

#include <framework/interfaces/ibaseline_types.h>

#include <memory>
#include <string>

namespace flowsql {
namespace baseline {

class RebuildTaskRuntime;

struct RebuildRequest {
    BaselineTaskKind task_kind = BaselineTaskKind::kValue;
    std::string task_id;
    std::string feature_name;
    std::string key;
    BaselineRebuildReason rebuild_reason = BaselineRebuildReason::kManual;
    int64_t bucket_start_hint = 0;
    int64_t bucket_end = 0;
    std::weak_ptr<RebuildTaskRuntime> runtime;
};

inline const char* RebuildReasonName(BaselineRebuildReason reason) {
    switch (reason) {
        case BaselineRebuildReason::kManual:
            return "manual";
        case BaselineRebuildReason::kShiftConfirmed:
            return "shift_confirmed";
        case BaselineRebuildReason::kScheduled:
            return "scheduled";
        case BaselineRebuildReason::kBootstrap:
            return "bootstrap";
    }
    return "unknown";
}

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_REBUILD_REBUILD_REQUEST_H_
