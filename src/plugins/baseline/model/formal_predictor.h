/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_MODEL_FORMAL_PREDICTOR_H_
#define _FLOWSQL_PLUGINS_BASELINE_MODEL_FORMAL_PREDICTOR_H_

#include <cstddef>
#include <cstdint>

#include "calendar_feature_helper.h"
#include "event_calendar_matcher.h"
#include "formal_model.h"

namespace flowsql {
namespace baseline {

enum class EventCalendarStatus : int32_t {
    kEnabled = 0,
    kDisabledNoTaskCalendar = 1,
    kDisabledNoModelCalendar = 2,
    kDisabledCalendarMismatch = 3,
};

struct FormalPredictContext {
    const BaselineTaskSpec* task_spec = nullptr;
    const CompiledEventCalendar* event_calendar = nullptr;
    int64_t bucket_id = 0;
};

struct FormalPredictFeatureView {
    // FormalPredictFeatureView 不拥有内存；父 batch 必须覆盖 view 的使用期。
    const LocalCalendarFeature* calendar = nullptr;
    const double* day_sin = nullptr;
    const double* day_cos = nullptr;
    std::size_t day_size = 0;
    const double* week_sin = nullptr;
    const double* week_cos = nullptr;
    std::size_t week_size = 0;
};

const char* EventCalendarStatusName(EventCalendarStatus status);

inline EventCalendarStatus EvaluateEventCalendarStatus(const FormalModelMetadata& metadata,
                                                       const CompiledEventCalendar* task_calendar) {
    if (!task_calendar) return EventCalendarStatus::kDisabledNoTaskCalendar;
    if (metadata.calendar_id.empty() || metadata.calendar_version.empty()) {
        return EventCalendarStatus::kDisabledNoModelCalendar;
    }
    if (metadata.calendar_id != task_calendar->calendar_id ||
        metadata.calendar_version != task_calendar->calendar_version) {
        return EventCalendarStatus::kDisabledCalendarMismatch;
    }
    return EventCalendarStatus::kEnabled;
}

struct FormalPrediction {
    bool ready = false;
    FormalModelKind model_kind = FormalModelKind::kNone;
    uint64_t model_version = 0;
    int64_t bucket_id = 0;
    double value = 0.0;
    double sigma_ref = 0.0;
    ModelReadiness readiness = ModelReadiness::kNotReady;
    double confidence_base = 0.0;
    bool event_enabled = false;
    EventCalendarStatus event_status = EventCalendarStatus::kDisabledNoTaskCalendar;
};

int PredictFormalModel(const ValueFormalModel* model,
                       const FormalPredictContext& context,
                       FormalPrediction* out);

int PredictFormalModelWithFeature(const ValueFormalModel* model,
                                  const FormalPredictContext& context,
                                  const FormalPredictFeatureView& feature,
                                  FormalPrediction* out);

int PredictFormalModel(const RatioFormalModel* model,
                       const FormalPredictContext& context,
                       FormalPrediction* out);

int PredictFormalModelWithFeature(const RatioFormalModel* model,
                                  const FormalPredictContext& context,
                                  const FormalPredictFeatureView& feature,
                                  FormalPrediction* out);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_MODEL_FORMAL_PREDICTOR_H_
