/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_MODEL_FORMAL_MODEL_STATE_H_
#define _FLOWSQL_PLUGINS_BASELINE_MODEL_FORMAL_MODEL_STATE_H_

#include <cstdint>
#include <string>

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

struct FormalModelState {
    bool formal_ready = false;
    uint64_t formal_model_version = 0;
    std::string formal_model_kind = "none";
    uint64_t candidate_generation = 0;
    uint64_t candidate_model_version = 0;
    std::string candidate_model_kind = "none";
    std::string candidate_state = "none";
    std::string switch_state = "none";
    double last_candidate_loss = 0.0;
    double last_incumbent_loss = 0.0;
    uint64_t last_validation_count = 0;
    ReplayWindowSummary last_replay_window;
    ReplayWindowSummary last_train_window;
    ReplayWindowSummary last_holdout_window;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_MODEL_FORMAL_MODEL_STATE_H_
