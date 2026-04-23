/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_MODEL_SERIES_STATE_H_
#define _FLOWSQL_PLUGINS_BASELINE_MODEL_SERIES_STATE_H_

#include <common/error_code.h>
#include <framework/interfaces/ibaseline_types.h>

#include <cstdint>

namespace flowsql {
namespace baseline {

enum class SeriesPersistenceMode : uint8_t {
    kByAnomaly = 0,
    kFreeze = 1,
};

struct SeriesUpdateResult {
    int32_t status = error::OK;
    uint32_t gap = 0;
    uint32_t persistence = 0;
    uint64_t flags = kBaselineFlagNone;
};

struct SeriesState {
    bool initialized = false;
    int64_t last_bucket_id = 0;
    uint32_t persistence = 0;
    uint64_t observation_count = 0;

    // 公共状态层只负责时序完整性、gap 惰性计算和持续性计数。
    // 具体的分数、方向和残差解释由 T1/T2/T3 各自算法层补充。
    SeriesUpdateResult ApplyObservation(int64_t bucket_id,
                                        SeriesPersistenceMode mode,
                                        bool is_anomalous) {
        SeriesUpdateResult result;

        if (initialized && bucket_id < last_bucket_id) {
            result.status = error::BAD_REQUEST;
            result.persistence = persistence;
            result.flags |= kBaselineFlagOutOfOrder;
            return result;
        }

        if (!initialized) {
            result.flags |= kBaselineFlagColdStart;
        } else if (bucket_id > last_bucket_id + 1) {
            result.gap = static_cast<uint32_t>(bucket_id - last_bucket_id - 1);
            result.flags |= kBaselineFlagGapBefore;
        }

        if (mode == SeriesPersistenceMode::kByAnomaly) {
            if (is_anomalous) {
                ++persistence;
            } else {
                persistence = 0;
            }
        }

        initialized = true;
        last_bucket_id = bucket_id;
        ++observation_count;

        result.persistence = persistence;
        return result;
    }

    SeriesUpdateResult ApplyObservation(int64_t bucket_id, bool is_anomalous) {
        return ApplyObservation(bucket_id, SeriesPersistenceMode::kByAnomaly, is_anomalous);
    }
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_MODEL_SERIES_STATE_H_
