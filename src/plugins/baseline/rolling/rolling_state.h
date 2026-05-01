/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_STATE_H_
#define _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_STATE_H_

#include <framework/interfaces/ibaseline_types.h>

#include <cstdint>
#include <string>
#include <vector>

#include "plugins/baseline/bootstrap/bootstrap_types.h"
#include "plugins/baseline/model/task_spec.h"
#include "plugins/baseline/rolling/observation_adapter.h"
#include "plugins/baseline/rolling/rolling_config.h"

namespace flowsql {
namespace baseline {

enum class RollingStateStatus : int32_t {
    kColdLearning = 0,
    kWarming = 1,
    kReadyHint = 2,
};

const char* RollingStateStatusName(RollingStateStatus status);

struct RollingHarmonicState {
    std::vector<double> sin_coeff;
    std::vector<double> cos_coeff;
    std::vector<double> sin_p;
    std::vector<double> cos_p;
};

struct RollingThetaState {
    double level = 0.0;
    double trend = 0.0;
    RollingHarmonicState daily;
    RollingHarmonicState weekly;
};

struct RollingState {
    std::string series_key;
    RollingThetaState theta;

    double sigma_init = 0.0;
    double sigma = 0.0;

    double p_level = 0.0;
    double p_level_trend = 0.0;
    double p_trend = 0.0;

    double short_ewma = 0.0;
    double long_ewma = 0.0;
    double drift_evidence = 0.0;

    RollingStateStatus state_status = RollingStateStatus::kColdLearning;
    bool has_seen_observation = false;
    int64_t last_seen_bucket = 0;
    uint64_t accepted_update_count = 0;
    double confidence = 0.0;
    std::string diagnostics;
};

RollingStateStatus StatusFromAcceptedUpdateCount(uint64_t accepted_update_count,
                                                 const BaselineRollingConfig& config);

BaselineStatus BuildEmptyRollingState(std::string_view series_key,
                                      const BaselineRollingConfig& config,
                                      RollingState* out);

BaselineStatus InitializeEmptyRollingStateFromObservation(
    const ObservedModelPoint& point,
    const BaselineRollingConfig& config,
    RollingState* state);

BaselineStatus InitializeRollingStateFromBootstrapSeed(const BaselineTaskSpec& spec,
                                                       std::string_view series_key,
                                                       const BootstrapSeed& seed,
                                                       const BaselineRollingConfig& config,
                                                       RollingState* out,
                                                       std::string* diagnostics = nullptr);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_STATE_H_
