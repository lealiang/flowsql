/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_MODEL_SERIES_STORE_H_
#define _FLOWSQL_PLUGINS_BASELINE_MODEL_SERIES_STORE_H_

#include <common/error_code.h>
#include <framework/interfaces/ibaseline_types.h>

#include <mutex>
#include <string>
#include <unordered_map>

#include "series_state.h"

namespace flowsql {
namespace baseline {

class SeriesStore {
 public:
    int ApplyObservation(const BaselineStringRef& key,
                         int64_t bucket_id,
                         SeriesPersistenceMode mode,
                         bool is_anomalous,
                         SeriesUpdateResult* out) {
        if (!out) return error::BAD_REQUEST;

        const std::string key_copy = CopyKey(key);
        if (key_copy.empty()) return error::BAD_REQUEST;

        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = states_[key_copy];
        *out = state.ApplyObservation(bucket_id, mode, is_anomalous);
        return out->status;
    }

    int ApplyObservation(const BaselineStringRef& key,
                         int64_t bucket_id,
                         bool is_anomalous,
                         SeriesUpdateResult* out) {
        if (!out) return error::BAD_REQUEST;

        const std::string key_copy = CopyKey(key);
        if (key_copy.empty()) return error::BAD_REQUEST;

        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = states_[key_copy];
        *out = state.ApplyObservation(bucket_id, SeriesPersistenceMode::kByAnomaly, is_anomalous);
        return out->status;
    }

    int GetState(const BaselineStringRef& key, SeriesState* out) const {
        if (!out) return error::BAD_REQUEST;

        const std::string key_copy = CopyKey(key);
        if (key_copy.empty()) return error::BAD_REQUEST;

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = states_.find(key_copy);
        if (it == states_.end()) return error::NOT_FOUND;
        *out = it->second;
        return error::OK;
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return states_.size();
    }

 private:
    static std::string CopyKey(const BaselineStringRef& key) {
        if (!key.data || key.size == 0) return "";
        return std::string(key.data, key.size);
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, SeriesState> states_;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_MODEL_SERIES_STORE_H_
