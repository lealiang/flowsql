/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_ROLLING_OBSERVATION_ADAPTER_H_
#define _FLOWSQL_PLUGINS_BASELINE_ROLLING_OBSERVATION_ADAPTER_H_

#include <framework/interfaces/ibaseline_types.h>

#include <cstdint>
#include <string>
#include <vector>

#include "plugins/baseline/model/task_spec.h"
#include "plugins/baseline/rolling/rolling_config.h"

namespace flowsql {
namespace baseline {

struct ObservedModelPoint {
    BaselineStatus status = BaselineStatus::kOk;
    std::string series_key;
    int64_t bucket_id = 0;

    double observed = 0.0;
    double y_model = 0.0;
    double extra_obs_noise_scale = 0.0;

    bool can_score = false;
    bool can_update = false;
    double score_weight = 0.0;
    double update_weight = 0.0;

    uint64_t sample_count = 0;
    bool skipped_low_sample_count = false;

    double numerator = 0.0;
    double denominator = 0.0;
    bool skipped_low_denominator = false;

    std::vector<std::string> uncertainty_source;
    std::string diagnostics;
};

ObservedModelPoint AdaptValueRollingObservation(const BaselineTaskSpec& spec,
                                                const BaselineRollingConfig& config,
                                                const ValueRollingObservation& obs);

ObservedModelPoint AdaptRatioRollingObservation(const BaselineTaskSpec& spec,
                                                const BaselineRollingConfig& config,
                                                const RatioRollingObservation& obs);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_ROLLING_OBSERVATION_ADAPTER_H_
