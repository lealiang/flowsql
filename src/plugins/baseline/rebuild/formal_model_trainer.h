/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_REBUILD_FORMAL_MODEL_TRAINER_H_
#define _FLOWSQL_PLUGINS_BASELINE_REBUILD_FORMAL_MODEL_TRAINER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "plugins/baseline/detector/ratio_detector_core.h"
#include "plugins/baseline/detector/value_detector_core.h"
#include "plugins/baseline/model/event_calendar_matcher.h"
#include "plugins/baseline/model/task_spec.h"
#include "plugins/baseline/rebuild/replay_runner.h"

namespace flowsql {
namespace baseline {

enum class FormalTrainFailureCode : int32_t {
    kNone = 0,
    kInsufficientTrainData = 1,
    kSolverUnavailable = 2,
    kTrainFailed = 3,
};

const char* FormalTrainFailureCodeName(FormalTrainFailureCode code);

struct ValueFormalTrainInput {
    const ValueFeatureProfile* profile = nullptr;
    const ValueReplaySeries* replay = nullptr;
    std::size_t train_count = 0;
    uint64_t model_version = 0;
    uint64_t holdout_count = 0;
    ReplayWindowSummary train_window;
    const BaselineTaskSpec* task_spec = nullptr;
    int64_t delta = 0;
    std::string tz;
    const CompiledEventCalendar* compiled_event_calendar = nullptr;
};

struct ValueFormalTrainResult {
    FormalTrainFailureCode failure = FormalTrainFailureCode::kTrainFailed;
    std::shared_ptr<ValueFormalModel> model;
};

struct RatioFormalTrainInput {
    const RatioFeatureProfile* profile = nullptr;
    const RatioReplaySeries* replay = nullptr;
    std::size_t train_count = 0;
    uint64_t model_version = 0;
    uint64_t holdout_count = 0;
    ReplayWindowSummary train_window;
    const BaselineTaskSpec* task_spec = nullptr;
    int64_t delta = 0;
    std::string tz;
    const CompiledEventCalendar* compiled_event_calendar = nullptr;
};

struct RatioFormalTrainResult {
    FormalTrainFailureCode failure = FormalTrainFailureCode::kTrainFailed;
    std::shared_ptr<RatioFormalModel> model;
};

class FormalModelTrainer {
 public:
    static FormalTrainFailureCode TrainValue(const ValueFormalTrainInput& input,
                                             ValueFormalTrainResult* out);
    static FormalTrainFailureCode TrainRatio(const RatioFormalTrainInput& input,
                                             RatioFormalTrainResult* out);
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_REBUILD_FORMAL_MODEL_TRAINER_H_
