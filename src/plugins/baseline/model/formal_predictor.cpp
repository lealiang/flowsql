/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "formal_predictor.h"

#include <common/error_code.h>

namespace flowsql {
namespace baseline {

namespace {

template <typename TModel>
int PredictFormalModelInternal(const TModel* model,
                               const EventCalendarSpec* task_calendar,
                               int64_t bucket_id,
                               double value,
                               double sigma_ref,
                               FormalPrediction* out) {
    if (!out) return error::BAD_REQUEST;

    *out = FormalPrediction{};
    out->bucket_id = bucket_id;
    if (!model || model->metadata.kind == FormalModelKind::kNone) {
        return error::UNAVAILABLE;
    }

    out->ready = true;
    out->model_kind = model->metadata.kind;
    out->model_version = model->metadata.model_version;
    out->value = value;
    out->sigma_ref = sigma_ref;
    out->event_status = EvaluateEventCalendarStatus(model->metadata, task_calendar);
    out->event_enabled = (out->event_status == EventCalendarStatus::kEnabled);
    return error::OK;
}

}  // namespace

const char* FormalModelKindName(FormalModelKind kind) {
    switch (kind) {
        case FormalModelKind::kValueInterceptFit:
            return "value_intercept_fit";
        case FormalModelKind::kRatioInterceptFit:
            return "ratio_intercept_fit";
        case FormalModelKind::kNone:
            break;
    }
    return "none";
}

const char* EventCalendarStatusName(EventCalendarStatus status) {
    switch (status) {
        case EventCalendarStatus::kEnabled:
            return "enabled";
        case EventCalendarStatus::kDisabledNoTaskCalendar:
            return "disabled_no_task_calendar";
        case EventCalendarStatus::kDisabledNoModelCalendar:
            return "disabled_no_model_calendar";
        case EventCalendarStatus::kDisabledCalendarMismatch:
            return "disabled_calendar_mismatch";
    }
    return "disabled_no_task_calendar";
}

int PredictFormalModel(const ValueFormalModel* model,
                       const EventCalendarSpec* task_calendar,
                       int64_t bucket_id,
                       FormalPrediction* out) {
    // v1 的正式模型先落为“常数截距项”。
    // 这里直接暴露训练得到的基线水平，后续再在相同 predictor 契约后面补趋势、
    // 周期和事件项，而不改热路径调用方式。
    return PredictFormalModelInternal(
        model,
        task_calendar,
        bucket_id,
        model ? model->intercept_x : 0.0,
        model ? model->sigma_ref : 0.0,
        out);
}

int PredictFormalModel(const RatioFormalModel* model,
                       const EventCalendarSpec* task_calendar,
                       int64_t bucket_id,
                       FormalPrediction* out) {
    return PredictFormalModelInternal(
        model, task_calendar, bucket_id, model ? model->intercept_ratio : 0.0, 0.0, out);
}

}  // namespace baseline
}  // namespace flowsql
