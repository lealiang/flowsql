/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "formal_predictor.h"

#include <common/error_code.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <unordered_set>

#include "plugins/baseline/model/calendar_feature_helper.h"
#include "plugins/baseline/model/profile_config.h"

namespace flowsql {
namespace baseline {

namespace {

constexpr double kPi = 3.14159265358979323846;

double RatioClipEps() {
    double eps = kRatioEpsLogit;
    (void)TryGetRatioGlobalNumericalOverride(&eps, nullptr, nullptr);
    return eps;
}

double Sigmoid(double value) {
    if (value >= 0.0) {
        const double z = std::exp(-value);
        return 1.0 / (1.0 + z);
    }
    const double z = std::exp(value);
    return z / (1.0 + z);
}

double ClipRatio(double value) {
    const double eps = RatioClipEps();
    return std::max(eps, std::min(1.0 - eps, value));
}

int64_t ResolveDelta(const ValueFormalModel& model, const FormalPredictContext& context) {
    if (model.delta > 0) return model.delta;
    if (context.task_spec && context.task_spec->delta > 0) return context.task_spec->delta;
    return 0;
}

int64_t ResolveDelta(const RatioFormalModel& model, const FormalPredictContext& context) {
    if (model.delta > 0) return model.delta;
    if (context.task_spec && context.task_spec->delta > 0) return context.task_spec->delta;
    return 0;
}

std::string ResolveTimezone(const std::string& model_tz, const FormalPredictContext& context) {
    if (!model_tz.empty()) return model_tz;
    if (context.task_spec && !context.task_spec->tz.empty()) return context.task_spec->tz;
    return "UTC";
}

double EvaluateFourier(const std::vector<double>& sin_coeff,
                       const std::vector<double>& cos_coeff,
                       double phase) {
    double value = 0.0;
    const std::size_t max_size = std::max(sin_coeff.size(), cos_coeff.size());
    for (std::size_t i = 0; i < max_size; ++i) {
        const double angle = 2.0 * kPi * static_cast<double>(i + 1) * phase;
        if (i < sin_coeff.size()) value += sin_coeff[i] * std::sin(angle);
        if (i < cos_coeff.size()) value += cos_coeff[i] * std::cos(angle);
    }
    return value;
}

double EvaluateFourierWithFeature(const std::vector<double>& sin_coeff,
                                  const std::vector<double>& cos_coeff,
                                  const double* sin_feature,
                                  const double* cos_feature,
                                  std::size_t feature_size) {
    double value = 0.0;
    if (sin_feature) {
        const std::size_t size = std::min(sin_coeff.size(), feature_size);
        for (std::size_t i = 0; i < size; ++i) {
            value += sin_coeff[i] * sin_feature[i];
        }
    }
    if (cos_feature) {
        const std::size_t size = std::min(cos_coeff.size(), feature_size);
        for (std::size_t i = 0; i < size; ++i) {
            value += cos_coeff[i] * cos_feature[i];
        }
    }
    return value;
}

double CenterAt(const std::vector<double>& centers, std::size_t index) {
    return index < centers.size() ? centers[index] : 0.0;
}

double EvaluateMonthPos(const MonthPosBlock& block,
                        int64_t bucket_id,
                        int64_t delta,
                        const std::string& tz) {
    if (!block.enabled || delta <= 0) return 0.0;

    double value = 0.0;
    const int32_t dom = DayOfMonthLocal(bucket_id, delta, tz);
    if (dom >= 1 && dom <= 31) {
        for (std::size_t i = 0; i < block.dom_coeff.size(); ++i) {
            const double indicator = (static_cast<int32_t>(i + 1) == dom) ? 1.0 : 0.0;
            value += block.dom_coeff[i] * (indicator - block.dom_center[i]);
        }
    }

    if (!block.dme_coeff.empty()) {
        const int32_t dme = DaysToMonthEndLocal(bucket_id, delta, tz);
        const std::size_t dme_index = static_cast<std::size_t>(
            std::max<int32_t>(0, std::min<int32_t>(dme, static_cast<int32_t>(block.dme_coeff.size() - 1))));
        for (std::size_t i = 0; i < block.dme_coeff.size(); ++i) {
            const double indicator = i == dme_index ? 1.0 : 0.0;
            value += block.dme_coeff[i] * (indicator - CenterAt(block.dme_center, i));
        }
    }

    std::tm local{};
    const bool is_last_weekday = ResolveLocalTime(bucket_id, delta, tz, &local) &&
                                 IsLastWeekdayOfMonthLocal(bucket_id, delta, tz);
    for (std::size_t i = 0; i < block.lwd_coeff.size(); ++i) {
        const double indicator =
            (is_last_weekday && static_cast<int32_t>(i) == local.tm_wday) ? 1.0 : 0.0;
        value += block.lwd_coeff[i] * (indicator - block.lwd_center[i]);
    }
    return value;
}

double EvaluateMonthPosWithFeature(const MonthPosBlock& block,
                                   const LocalCalendarFeature* feature) {
    if (!block.enabled || !feature || !feature->valid) return 0.0;

    double value = 0.0;
    const int32_t dom = feature->day_of_month;
    if (dom >= 1 && dom <= 31) {
        for (std::size_t i = 0; i < block.dom_coeff.size(); ++i) {
            const double indicator = (static_cast<int32_t>(i + 1) == dom) ? 1.0 : 0.0;
            value += block.dom_coeff[i] * (indicator - block.dom_center[i]);
        }
    }

    if (!block.dme_coeff.empty()) {
        const std::size_t dme_index = static_cast<std::size_t>(
            std::max<int32_t>(
                0,
                std::min<int32_t>(
                    feature->days_to_month_end,
                    static_cast<int32_t>(block.dme_coeff.size() - 1))));
        for (std::size_t i = 0; i < block.dme_coeff.size(); ++i) {
            const double indicator = i == dme_index ? 1.0 : 0.0;
            value += block.dme_coeff[i] * (indicator - CenterAt(block.dme_center, i));
        }
    }

    for (std::size_t i = 0; i < block.lwd_coeff.size(); ++i) {
        const double indicator =
            (feature->is_last_weekday_of_month && static_cast<int32_t>(i) == feature->weekday)
                ? 1.0
                : 0.0;
        value += block.lwd_coeff[i] * (indicator - block.lwd_center[i]);
    }
    return value;
}

double EvaluateCore(const CoreBlock& block,
                    int64_t bucket_id,
                    int64_t train_start,
                    int64_t delta,
                    const std::string& tz) {
    double value = block.beta0 + block.trend_k * static_cast<double>(bucket_id - train_start);
    if (delta > 0) {
        value += EvaluateFourier(block.day_sin, block.day_cos, PhaseDayLocal(bucket_id, delta, tz));
        value += EvaluateFourier(block.week_sin, block.week_cos, PhaseWeekLocal(bucket_id, delta, tz));
    }
    return value;
}

double EvaluateCoreWithFeature(const CoreBlock& block,
                               int64_t bucket_id,
                               int64_t train_start,
                               const FormalPredictFeatureView& feature) {
    double value = block.beta0 + block.trend_k * static_cast<double>(bucket_id - train_start);
    value += EvaluateFourierWithFeature(
        block.day_sin, block.day_cos, feature.day_sin, feature.day_cos, feature.day_size);
    value += EvaluateFourierWithFeature(block.week_sin,
                                        block.week_cos,
                                        feature.week_sin,
                                        feature.week_cos,
                                        feature.week_size);
    return value;
}

double EvaluateEventBlock(const EventBlock& block,
                          const FormalPredictContext& context,
                          EventCalendarStatus event_status) {
    if (!block.enabled || event_status != EventCalendarStatus::kEnabled ||
        !context.task_spec || !context.event_calendar) {
        return 0.0;
    }

    const std::vector<std::string> hit_events =
        ResolveBucketEvents(*context.event_calendar, *context.task_spec, context.bucket_id);
    if (hit_events.empty()) return 0.0;
    const std::unordered_set<std::string> hit_set(hit_events.begin(), hit_events.end());

    double value = 0.0;
    const std::size_t count = std::min(block.active_event_codes.size(), block.coeff.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (hit_set.find(block.active_event_codes[i]) != hit_set.end()) {
            value += block.coeff[i];
        }
    }
    return value;
}

template <typename TModel>
void FillCommonPrediction(const TModel& model,
                          const FormalPredictContext& context,
                          FormalPrediction* out) {
    out->ready = true;
    out->model_kind = model.metadata.kind;
    out->model_version = model.metadata.model_version;
    out->bucket_id = context.bucket_id;
    out->readiness = model.readiness;
    out->confidence_base = model.confidence_base_at_train;
    out->event_status = EvaluateEventCalendarStatus(model.metadata, context.event_calendar);
    out->event_enabled = out->event_status == EventCalendarStatus::kEnabled;
}

}  // namespace

const char* FormalModelKindName(FormalModelKind kind) {
    switch (kind) {
        case FormalModelKind::kValueBaseline:
            return "value_baseline";
        case FormalModelKind::kRatioBaseline:
            return "ratio_baseline";
        case FormalModelKind::kNone:
            break;
    }
    return "none";
}

const char* TransformKindName(TransformKind kind) {
    switch (kind) {
        case TransformKind::kIdentity:
            return "identity";
        case TransformKind::kLog1p:
            return "log1p";
        case TransformKind::kLogit:
            return "logit";
    }
    return "identity";
}

TransformKind ParseTransformKind(const std::string& name) {
    if (name == "log1p") return TransformKind::kLog1p;
    if (name == "logit") return TransformKind::kLogit;
    return TransformKind::kIdentity;
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
                       const FormalPredictContext& context,
                       FormalPrediction* out) {
    if (!out) return error::BAD_REQUEST;
    *out = FormalPrediction{};
    if (!model || model->metadata.kind != FormalModelKind::kValueBaseline) {
        return error::UNAVAILABLE;
    }

    FillCommonPrediction(*model, context, out);
    const int64_t delta = ResolveDelta(*model, context);
    const std::string tz = ResolveTimezone(model->tz, context);
    double value = EvaluateCore(model->core_block, context.bucket_id, model->train_start, delta, tz);
    value += EvaluateMonthPos(model->monthpos_block, context.bucket_id, delta, tz);
    value += EvaluateEventBlock(model->event_block, context, out->event_status);
    out->value = value;
    out->sigma_ref = model->sigma_ref;
    return error::OK;
}

int PredictFormalModelWithFeature(const ValueFormalModel* model,
                                  const FormalPredictContext& context,
                                  const FormalPredictFeatureView& feature,
                                  FormalPrediction* out) {
    if (!out) return error::BAD_REQUEST;
    *out = FormalPrediction{};
    if (!model || model->metadata.kind != FormalModelKind::kValueBaseline) {
        return error::UNAVAILABLE;
    }
    if (!feature.calendar || !feature.calendar->valid ||
        feature.calendar->bucket_id != context.bucket_id) {
        return error::BAD_REQUEST;
    }

    FillCommonPrediction(*model, context, out);
    double value =
        EvaluateCoreWithFeature(model->core_block, context.bucket_id, model->train_start, feature);
    value += EvaluateMonthPosWithFeature(model->monthpos_block, feature.calendar);
    value += EvaluateEventBlock(model->event_block, context, out->event_status);
    out->value = value;
    out->sigma_ref = model->sigma_ref;
    return error::OK;
}

int PredictFormalModel(const RatioFormalModel* model,
                       const FormalPredictContext& context,
                       FormalPrediction* out) {
    if (!out) return error::BAD_REQUEST;
    *out = FormalPrediction{};
    if (!model || model->metadata.kind != FormalModelKind::kRatioBaseline) {
        return error::UNAVAILABLE;
    }

    FillCommonPrediction(*model, context, out);
    const int64_t delta = ResolveDelta(*model, context);
    const std::string tz = ResolveTimezone(model->tz, context);
    double eta = EvaluateCore(model->core_block, context.bucket_id, model->train_start, delta, tz);
    eta += EvaluateMonthPos(model->monthpos_block, context.bucket_id, delta, tz);
    eta += EvaluateEventBlock(model->event_block, context, out->event_status);
    out->value = ClipRatio(Sigmoid(eta));
    out->sigma_ref = 0.0;
    return error::OK;
}

int PredictFormalModelWithFeature(const RatioFormalModel* model,
                                  const FormalPredictContext& context,
                                  const FormalPredictFeatureView& feature,
                                  FormalPrediction* out) {
    if (!out) return error::BAD_REQUEST;
    *out = FormalPrediction{};
    if (!model || model->metadata.kind != FormalModelKind::kRatioBaseline) {
        return error::UNAVAILABLE;
    }
    if (!feature.calendar || !feature.calendar->valid ||
        feature.calendar->bucket_id != context.bucket_id) {
        return error::BAD_REQUEST;
    }

    FillCommonPrediction(*model, context, out);
    double eta =
        EvaluateCoreWithFeature(model->core_block, context.bucket_id, model->train_start, feature);
    eta += EvaluateMonthPosWithFeature(model->monthpos_block, feature.calendar);
    eta += EvaluateEventBlock(model->event_block, context, out->event_status);
    out->value = ClipRatio(Sigmoid(eta));
    out->sigma_ref = 0.0;
    return error::OK;
}

}  // namespace baseline
}  // namespace flowsql
