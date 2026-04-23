/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "formal_model_trainer.h"

#include <common/error_code.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "plugins/baseline/solver/solver_backend.h"

namespace flowsql {
namespace baseline {

namespace {

constexpr double kT1SigmaRefFloor = 1e-3;

template <typename TModel>
void FillCommonMetadata(uint64_t model_version,
                        uint64_t holdout_count,
                        const ReplayWindowSummary& train_window,
                        const EventCalendarSpec* event_calendar_spec,
                        TModel* model) {
    if (!model) return;
    model->metadata.model_version = model_version;
    model->metadata.holdout_count = holdout_count;
    model->metadata.observation_count = train_window.observation_count;
    model->metadata.train_bucket_start = train_window.first_bucket_id;
    model->metadata.train_bucket_end = train_window.last_bucket_id;
    if (event_calendar_spec) {
        model->metadata.calendar_id = event_calendar_spec->calendar_id;
        model->metadata.calendar_version = event_calendar_spec->calendar_version;
    } else {
        model->metadata.calendar_id.clear();
        model->metadata.calendar_version.clear();
    }
}

double UpperMedian(std::vector<double>* values) {
    if (!values || values->empty()) return 0.0;

    auto middle = values->begin() + static_cast<std::ptrdiff_t>(values->size() / 2);
    std::nth_element(values->begin(), middle, values->end());
    return *middle;
}

double ComputeValueSigmaRef(const std::vector<double>& x_values,
                            double intercept_x) {
    if (x_values.empty()) return kT1SigmaRefFloor;

    // `sigma_ref` 在变换后的 x 空间里按 MAD 估计，保持对尖峰异常的鲁棒性。
    // 这和 design 里的“用稳健残差尺度做标准化”是同一语义。
    std::vector<double> residuals;
    residuals.reserve(x_values.size());
    for (double x : x_values) {
        residuals.push_back(x - intercept_x);
    }

    const double median_residual = UpperMedian(&residuals);

    std::vector<double> abs_deviation;
    abs_deviation.reserve(residuals.size());
    for (double residual : residuals) {
        abs_deviation.push_back(std::fabs(residual - median_residual));
    }

    return std::max(kT1SigmaRefFloor, 1.4826 * UpperMedian(&abs_deviation));
}

}  // namespace

const char* FormalTrainFailureCodeName(FormalTrainFailureCode code) {
    switch (code) {
        case FormalTrainFailureCode::kNone:
            return "none";
        case FormalTrainFailureCode::kInsufficientTrainData:
            return "insufficient_train_data";
        case FormalTrainFailureCode::kSolverUnavailable:
            return "solver_unavailable";
        case FormalTrainFailureCode::kTrainFailed:
            return "train_failed";
    }
    return "train_failed";
}

FormalTrainFailureCode FormalModelTrainer::TrainValue(const ValueFormalTrainInput& input,
                                                      ValueFormalTrainResult* out) {
    if (!out) return FormalTrainFailureCode::kTrainFailed;
    *out = ValueFormalTrainResult{};

    if (!input.profile || !input.replay || input.train_count > input.replay->points.size()) {
        return out->failure = FormalTrainFailureCode::kTrainFailed;
    }
    if (input.train_count < 2) {
        return out->failure = FormalTrainFailureCode::kInsufficientTrainData;
    }
    if (!SolverBackend::IsAvailable()) {
        return out->failure = FormalTrainFailureCode::kSolverUnavailable;
    }

    std::vector<double> x_values;
    std::vector<double> weights;
    x_values.reserve(input.train_count);
    weights.reserve(input.train_count);

    for (std::size_t i = 0; i < input.train_count; ++i) {
        x_values.push_back(TransformValueObservation(*input.profile,
                                                     input.replay->points[i].value));
        weights.push_back(1.0);
    }

    // v1 的 T1 formal 先收口为“变换空间里的加权常数截距项 + 鲁棒 sigma_ref”。
    // 这样先把 predictor / rebuild / validation 契约跑通，后续再在相同接口后面补趋势和季节项。
    WeightedInterceptFitResult fit;
    if (SolverBackend::FitWeightedIntercept(
            x_values.data(), weights.data(), x_values.size(), &fit) != error::OK) {
        return out->failure = FormalTrainFailureCode::kTrainFailed;
    }

    auto model = std::make_shared<ValueFormalModel>();
    model->metadata.kind = FormalModelKind::kValueInterceptFit;
    FillCommonMetadata(input.model_version,
                       input.holdout_count,
                       input.train_window,
                       input.event_calendar_spec,
                       model.get());
    model->intercept_x = fit.intercept;
    model->sigma_ref = ComputeValueSigmaRef(x_values, fit.intercept);

    out->failure = FormalTrainFailureCode::kNone;
    out->model = std::move(model);
    return out->failure;
}

FormalTrainFailureCode FormalModelTrainer::TrainRatio(const RatioFormalTrainInput& input,
                                                      RatioFormalTrainResult* out) {
    if (!out) return FormalTrainFailureCode::kTrainFailed;
    *out = RatioFormalTrainResult{};

    if (!input.profile || !input.replay || input.train_count > input.replay->points.size()) {
        return out->failure = FormalTrainFailureCode::kTrainFailed;
    }
    if (input.train_count < 2) {
        return out->failure = FormalTrainFailureCode::kInsufficientTrainData;
    }
    if (!SolverBackend::IsAvailable()) {
        return out->failure = FormalTrainFailureCode::kSolverUnavailable;
    }

    std::vector<double> ratios;
    std::vector<double> weights;
    ratios.reserve(input.train_count);
    weights.reserve(input.train_count);

    for (std::size_t i = 0; i < input.train_count; ++i) {
        const auto& point = input.replay->points[i];
        ratios.push_back(point.numerator / point.denominator);
        weights.push_back(point.denominator);
    }

    // v1 的 T2 formal 同样只训练截距项，但用 denominator 做权重，
    // 让高分母 bucket 对基线水平的贡献更大，符合 T2 的方差层语义。
    WeightedInterceptFitResult fit;
    if (SolverBackend::FitWeightedIntercept(
            ratios.data(), weights.data(), ratios.size(), &fit) != error::OK) {
        return out->failure = FormalTrainFailureCode::kTrainFailed;
    }

    auto model = std::make_shared<RatioFormalModel>();
    model->metadata.kind = FormalModelKind::kRatioInterceptFit;
    FillCommonMetadata(input.model_version,
                       input.holdout_count,
                       input.train_window,
                       input.event_calendar_spec,
                       model.get());
    model->intercept_ratio = fit.intercept;

    out->failure = FormalTrainFailureCode::kNone;
    out->model = std::move(model);
    return out->failure;
}

}  // namespace baseline
}  // namespace flowsql
