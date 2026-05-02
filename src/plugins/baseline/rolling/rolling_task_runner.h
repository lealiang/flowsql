/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_TASK_RUNNER_H_
#define _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_TASK_RUNNER_H_

#include <framework/interfaces/ibaseline_types.h>

#include <string>
#include <string_view>
#include <unordered_map>

#include "plugins/baseline/task/bootstrap_task_store.h"
#include "plugins/baseline/rolling/rolling_state.h"

namespace flowsql {
namespace baseline {

using RollingStateMap = std::unordered_map<std::string, RollingState>;

RollingBaselineResult RunValueRollingSubmit(const BaselineTaskSpec& spec,
                                            const BootstrapSeedStore& seeds,
                                            RollingStateMap* states,
                                            const ValueRollingObservation& obs,
                                            const RollingSubmitOptions& options);

RollingBaselineResult RunRatioRollingSubmit(const BaselineTaskSpec& spec,
                                            const BootstrapSeedStore& seeds,
                                            RollingStateMap* states,
                                            const RatioRollingObservation& obs,
                                            const RollingSubmitOptions& options);

RollingPrediction PredictRollingForSeries(const BaselineTaskSpec& spec,
                                          const BootstrapSeedStore& seeds,
                                          const RollingStateMap& states,
                                          std::string_view series_key,
                                          int64_t bucket_id);

struct RollingWarmupStats {
    uint64_t success_count = 0;
    uint64_t failure_count = 0;
    uint64_t skipped_existing_count = 0;
};

RollingWarmupStats WarmupRollingStatesFromBootstrapSeeds(const BaselineTaskSpec& spec,
                                                         const BootstrapSeedStore& seeds,
                                                         RollingStateMap* states);

BaselineSerializationResult QueryRollingTaskSnapshot(const BaselineTaskSpec& spec,
                                                     const RollingStateMap& states,
                                                     BaselineSerializationFormat format);

BaselineSerializationResult QueryRollingSeriesSnapshot(const BaselineTaskSpec& spec,
                                                       const RollingStateMap& states,
                                                       std::string_view series_key,
                                                       BaselineSerializationFormat format);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_TASK_RUNNER_H_
