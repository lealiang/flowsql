/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_MODEL_SERIES_OVERRIDE_H_
#define _FLOWSQL_PLUGINS_BASELINE_MODEL_SERIES_OVERRIDE_H_

#include <framework/interfaces/ibaseline_types.h>

#include <cstdint>
#include <string>
#include <vector>

namespace flowsql {
namespace baseline {

struct BaselineSourceRef {
    std::string source_key;
};

struct BaselineSourceConfig {
    std::vector<BaselineSourceRef> sources;

    bool empty() const { return sources.empty(); }
};

struct SeriesBaselineSourceConfig {
    std::string key;
    BaselineSourceConfig config;
};

struct BaselineSourceDecision {
    BaselineSourceKind selected_kind = BaselineSourceKind::kNone;
    std::string selected_source_key;
    bool serviceable = false;
};

inline const char* BaselineSourceKindName(BaselineSourceKind kind) {
    switch (kind) {
        case BaselineSourceKind::kSelf:
            return "self";
        case BaselineSourceKind::kConfiguredSource:
            return "configured_source";
        case BaselineSourceKind::kNone:
            break;
    }
    return "none";
}

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_MODEL_SERIES_OVERRIDE_H_
