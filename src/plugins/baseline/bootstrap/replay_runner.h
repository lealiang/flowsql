/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_BOOTSTRAP_REPLAY_RUNNER_H_
#define _FLOWSQL_PLUGINS_BASELINE_BOOTSTRAP_REPLAY_RUNNER_H_

#include <cstdint>
#include <string>
#include <vector>

namespace flowsql {
namespace baseline {

struct ReplayWindowSummary {
    bool has_data = false;
    uint64_t observation_count = 0;
    int64_t first_bucket_id = 0;
    int64_t last_bucket_id = 0;
    int64_t request_bucket_start = 0;
    int64_t request_bucket_end = 0;
};

struct ValueReplayPoint {
    int64_t bucket_id = 0;
    double value = 0.0;
    uint64_t sample_count = 0;
};

struct ValueReplaySeries {
    std::string key;
    ReplayWindowSummary window;
    std::vector<ValueReplayPoint> points;
};

struct RatioReplayPoint {
    int64_t bucket_id = 0;
    double numerator = 0.0;
    double denominator = 0.0;
};

struct RatioReplaySeries {
    std::string key;
    ReplayWindowSummary window;
    std::vector<RatioReplayPoint> points;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_BOOTSTRAP_REPLAY_RUNNER_H_
