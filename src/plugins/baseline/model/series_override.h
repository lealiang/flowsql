/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_MODEL_SERIES_OVERRIDE_H_
#define _FLOWSQL_PLUGINS_BASELINE_MODEL_SERIES_OVERRIDE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace flowsql {
namespace baseline {

struct BaselineSourceRef {
    std::string source_key;
};

using BaselineSourceConfig = std::vector<BaselineSourceRef>;

struct SeriesOverride {
    std::string key;
    BaselineSourceConfig baseline_sources;
};

enum class BaselineSourceDecisionKind : uint8_t {
    kSelf = 0,
    kConfiguredSource = 1,
    kNone = 2,
};

struct BaselineSourceDecision {
    BaselineSourceDecisionKind kind = BaselineSourceDecisionKind::kNone;
    std::string source_key;
};

inline const char* BaselineSourceDecisionKindName(BaselineSourceDecisionKind kind) {
    switch (kind) {
        case BaselineSourceDecisionKind::kSelf:
            return "self";
        case BaselineSourceDecisionKind::kConfiguredSource:
            return "configured_source";
        case BaselineSourceDecisionKind::kNone:
            break;
    }
    return "none";
}

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_MODEL_SERIES_OVERRIDE_H_
