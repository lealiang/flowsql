/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_BOOTSTRAP_FORMAL_MODEL_TRAINER_H_
#define _FLOWSQL_PLUGINS_BASELINE_BOOTSTRAP_FORMAL_MODEL_TRAINER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "plugins/baseline/model/event_calendar_matcher.h"
#include "plugins/baseline/model/formal_model.h"
#include "plugins/baseline/model/task_spec.h"
#include "plugins/baseline/bootstrap/replay_runner.h"

#include <cmath>

namespace flowsql {
namespace baseline {

enum class FormalTrainFailureCode : int32_t {
    kNone = 0,
    kInsufficientTrainData = 1,
    kSolverUnavailable = 2,
    kTrainFailed = 3,
};

const char* FormalTrainFailureCodeName(FormalTrainFailureCode code);

struct ValueFeatureProfile {
    std::string profile;
    std::string transform_name = "log1p";
    bool is_sampled = false;
    uint32_t n_train_min = 0;
    double kappa_sample = 0.0;
};

inline double TransformValuePoint(const ValueFeatureProfile& profile,
                                  double value) {
    if (profile.transform_name == "identity") return value;
    return std::log1p(value);
}

struct RatioFeatureProfile {
    std::string profile;
    uint32_t d_min_train = 0;
    uint32_t d_score_min = 0;
    uint32_t d_shift_min = 0;
    double kappa_den = 0.0;
    double s_prior = 0.0;
    double phi_over = 1.0;
};

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
    bool enable_monthpos = true;
    bool enable_event = true;
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
    bool enable_monthpos = true;
    bool enable_event = true;
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

#endif  // _FLOWSQL_PLUGINS_BASELINE_BOOTSTRAP_FORMAL_MODEL_TRAINER_H_
