/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_REBUILD_REPLAY_RUNNER_H_
#define _FLOWSQL_PLUGINS_BASELINE_REBUILD_REPLAY_RUNNER_H_

#include <framework/interfaces/ibaseline_types.h>

#include <cstdint>
#include <string>
#include <vector>

#include "plugins/baseline/model/formal_model_state.h"

namespace flowsql {
namespace baseline {

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

class ValueReplayRunner {
 public:
    explicit ValueReplayRunner(std::string expected_key);

    int Push(const ValueObservation& obs);
    void Finalize(int64_t request_bucket_start,
                  int64_t request_bucket_end,
                  ValueReplaySeries* out) const;

 private:
    std::string expected_key_;
    ValueReplaySeries series_;
};

class RatioReplayRunner {
 public:
    explicit RatioReplayRunner(std::string expected_key);

    int Push(const RatioObservation& obs);
    void Finalize(int64_t request_bucket_start,
                  int64_t request_bucket_end,
                  RatioReplaySeries* out) const;

 private:
    std::string expected_key_;
    RatioReplaySeries series_;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_REBUILD_REPLAY_RUNNER_H_
