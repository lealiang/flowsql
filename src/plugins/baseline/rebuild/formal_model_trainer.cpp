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
#include <cstddef>
#include <set>
#include <utility>
#include <vector>

#include "plugins/baseline/model/calendar_feature_helper.h"
#include "plugins/baseline/model/profile_config.h"
#include "plugins/baseline/model/readiness_helper.h"
#include "plugins/baseline/solver/solver_backend.h"

namespace flowsql {
namespace baseline {

namespace {

constexpr double kSigmaFloor = 1e-3;
constexpr double kPi = 3.14159265358979323846;

struct ValueTrainRow {
    int64_t bucket_id = 0;
    double x = 0.0;
    double weight = 1.0;
};

struct RatioTrainRow {
    int64_t bucket_id = 0;
    double eta = 0.0;
    double weight = 1.0;
};

template <typename TModel>
void FillCommonMetadata(uint64_t model_version,
                        uint64_t holdout_count,
                        const ReplayWindowSummary& train_window,
                        const CompiledEventCalendar* compiled_event_calendar,
                        TModel* model) {
    if (!model) return;
    model->metadata.model_version = model_version;
    model->metadata.holdout_count = holdout_count;
    model->metadata.observation_count = train_window.observation_count;
    model->metadata.train_bucket_start = train_window.first_bucket_id;
    model->metadata.train_bucket_end = train_window.last_bucket_id;
    if (compiled_event_calendar) {
        model->metadata.calendar_id = compiled_event_calendar->calendar_id;
        model->metadata.calendar_version = compiled_event_calendar->calendar_version;
    } else {
        model->metadata.calendar_id.clear();
        model->metadata.calendar_version.clear();
    }
}

FitBlockDigest BuildDigest(const std::string& block_name,
                           const char* status,
                           uint64_t sample_count,
                           double objective,
                           double condition_est) {
    FitBlockDigest digest;
    digest.block_name = block_name;
    digest.status = status;
    digest.sample_count = sample_count;
    digest.objective = objective;
    digest.condition_est = condition_est;
    return digest;
}

double UpperMedian(std::vector<double>* values) {
    if (!values || values->empty()) return 0.0;
    auto middle = values->begin() + static_cast<std::ptrdiff_t>(values->size() / 2);
    std::nth_element(values->begin(), middle, values->end());
    return *middle;
}

double EstimateSigmaMAD(const std::vector<double>& residual) {
    if (residual.empty()) return kSigmaFloor;

    std::vector<double> centered = residual;
    const double median = UpperMedian(&centered);
    std::vector<double> deviation;
    deviation.reserve(residual.size());
    for (double value : residual) {
        deviation.push_back(std::fabs(value - median));
    }
    return std::max(kSigmaFloor, 1.4826 * UpperMedian(&deviation));
}

int64_t ResolveDelta(const ValueFormalTrainInput& input) {
    if (input.delta > 0) return input.delta;
    if (input.task_spec && input.task_spec->delta > 0) return input.task_spec->delta;
    return 0;
}

int64_t ResolveDelta(const RatioFormalTrainInput& input) {
    if (input.delta > 0) return input.delta;
    if (input.task_spec && input.task_spec->delta > 0) return input.task_spec->delta;
    return 0;
}

std::string ResolveTimezone(const ValueFormalTrainInput& input) {
    if (!input.tz.empty()) return input.tz;
    if (input.task_spec && !input.task_spec->tz.empty()) return input.task_spec->tz;
    return "UTC";
}

std::string ResolveTimezone(const RatioFormalTrainInput& input) {
    if (!input.tz.empty()) return input.tz;
    if (input.task_spec && !input.task_spec->tz.empty()) return input.task_spec->tz;
    return "UTC";
}

TrainingCoverageStats BuildTrainingCoverageStats(const std::vector<int64_t>& bucket_ids,
                                                 int64_t delta,
                                                 const std::string& tz) {
    TrainingCoverageStats stats;
    if (bucket_ids.empty()) return stats;

    stats.valid_bucket_count = static_cast<uint64_t>(bucket_ids.size());
    stats.first_bucket_id = bucket_ids.front();
    stats.last_bucket_id = bucket_ids.back();
    stats.total_bucket_span = static_cast<uint64_t>(bucket_ids.back() - bucket_ids.front() + 1);

    std::set<int32_t> months;
    for (int64_t bucket_id : bucket_ids) {
        std::tm local{};
        if (!ResolveLocalTime(bucket_id, delta, tz, &local)) continue;
        months.insert((local.tm_year + 1900) * 100 + (local.tm_mon + 1));
    }
    stats.month_count = static_cast<uint32_t>(months.size());
    return stats;
}

void AppendCoreRow(const SharedProfileConfig& shared_config,
                   int64_t bucket_id,
                   int64_t train_start,
                   int64_t delta,
                   const std::string& tz,
                   std::vector<double>* row) {
    if (!row) return;
    row->push_back(1.0);
    row->push_back(static_cast<double>(bucket_id - train_start));
    for (int32_t m = 1; m <= shared_config.k_day; ++m) {
        const double angle = 2.0 * kPi * static_cast<double>(m) *
                             PhaseDayLocal(bucket_id, delta, tz);
        row->push_back(std::sin(angle));
        row->push_back(std::cos(angle));
    }
    for (int32_t m = 1; m <= shared_config.k_week; ++m) {
        const double angle = 2.0 * kPi * static_cast<double>(m) *
                             PhaseWeekLocal(bucket_id, delta, tz);
        row->push_back(std::sin(angle));
        row->push_back(std::cos(angle));
    }
}

BlockFitSpec BuildCoreFitSpec(const std::vector<ValueTrainRow>& rows,
                              const SharedProfileConfig& shared_config,
                              int64_t delta,
                              const std::string& tz) {
    BlockFitSpec spec;
    spec.block_name = "core";
    spec.row_count = rows.size();
    spec.col_count =
        2 + static_cast<std::size_t>(shared_config.k_day + shared_config.k_week) * 2;
    spec.y_target.reserve(rows.size());
    spec.x_matrix.reserve(rows.size() * spec.col_count);
    spec.sample_weight.reserve(rows.size());
    spec.ridge_diag.assign(spec.col_count, 0.0);
    spec.col_roles.reserve(spec.col_count);

    spec.col_roles.push_back(BlockColumnRole::kIntercept);
    spec.col_roles.push_back(BlockColumnRole::kTrend);
    for (int32_t i = 0; i < shared_config.k_day; ++i) {
        spec.col_roles.push_back(BlockColumnRole::kDaySin);
        spec.col_roles.push_back(BlockColumnRole::kDayCos);
    }
    for (int32_t i = 0; i < shared_config.k_week; ++i) {
        spec.col_roles.push_back(BlockColumnRole::kWeekSin);
        spec.col_roles.push_back(BlockColumnRole::kWeekCos);
    }
    for (std::size_t i = 2; i < spec.col_count; ++i) {
        spec.ridge_diag[i] = shared_config.lambda_season;
    }

    const int64_t train_start = rows.front().bucket_id;
    for (const auto& row : rows) {
        spec.y_target.push_back(row.x);
        spec.sample_weight.push_back(row.weight);
        AppendCoreRow(shared_config, row.bucket_id, train_start, delta, tz, &spec.x_matrix);
    }
    return spec;
}

void ApplyCoreBeta(const SharedProfileConfig& shared_config,
                   const std::vector<double>& beta,
                   CoreBlock* block) {
    if (!block) return;
    block->beta0 = beta.empty() ? 0.0 : beta[0];
    block->trend_k = beta.size() >= 2 ? beta[1] : 0.0;
    block->day_sin.assign(shared_config.k_day, 0.0);
    block->day_cos.assign(shared_config.k_day, 0.0);
    block->week_sin.assign(shared_config.k_week, 0.0);
    block->week_cos.assign(shared_config.k_week, 0.0);

    std::size_t index = 2;
    for (int32_t i = 0; i < shared_config.k_day; ++i) {
        if (index < beta.size()) block->day_sin[i] = beta[index++];
        if (index < beta.size()) block->day_cos[i] = beta[index++];
    }
    for (int32_t i = 0; i < shared_config.k_week; ++i) {
        if (index < beta.size()) block->week_sin[i] = beta[index++];
        if (index < beta.size()) block->week_cos[i] = beta[index++];
    }
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

double EvaluateCore(const CoreBlock& block,
                    int64_t bucket_id,
                    int64_t train_start,
                    int64_t delta,
                    const std::string& tz) {
    double value = block.beta0 + block.trend_k * static_cast<double>(bucket_id - train_start);
    if (delta > 0) {
        value += EvaluateFourier(block.day_sin, block.day_cos, PhaseDayLocal(bucket_id, delta, tz));
        value +=
            EvaluateFourier(block.week_sin, block.week_cos, PhaseWeekLocal(bucket_id, delta, tz));
    }
    return value;
}

struct MonthPosDesign {
    std::vector<double> x_matrix;
    std::vector<double> ridge_diag;
    std::vector<double> y_target;
    std::vector<double> sample_weight;
    std::array<double, 31> dom_center{};
    std::vector<double> dme_center;
    std::array<double, 7> lwd_center{};
    std::size_t row_count = 0;
    std::size_t col_count = 0;
};

MonthPosDesign BuildMonthPosDesign(const std::vector<ValueTrainRow>& rows,
                                   const SharedProfileConfig& shared_config,
                                   const CoreBlock& core_block,
                                   int64_t train_start,
                                   int64_t delta,
                                   const std::string& tz) {
    MonthPosDesign design;
    design.row_count = rows.size();
    design.col_count = 31 + static_cast<std::size_t>(shared_config.dme_max + 1) + 7;
    design.dme_center.assign(shared_config.dme_max + 1, 0.0);
    design.ridge_diag.assign(design.col_count, 0.0);
    design.y_target.reserve(rows.size());
    design.sample_weight.reserve(rows.size());
    design.x_matrix.reserve(rows.size() * design.col_count);

    for (std::size_t i = 0; i < 31; ++i) {
        design.ridge_diag[i] = shared_config.lambda_dom;
    }
    for (std::size_t i = 31; i < 31 + design.dme_center.size(); ++i) {
        design.ridge_diag[i] = shared_config.lambda_dme;
    }
    for (std::size_t i = 31 + design.dme_center.size(); i < design.col_count; ++i) {
        design.ridge_diag[i] = shared_config.lambda_lwd;
    }

    std::vector<std::vector<double>> raw_rows;
    raw_rows.reserve(rows.size());
    for (const auto& row : rows) {
        std::vector<double> raw(design.col_count, 0.0);
        const int32_t dom = DayOfMonthLocal(row.bucket_id, delta, tz);
        if (dom >= 1 && dom <= 31) raw[static_cast<std::size_t>(dom - 1)] = 1.0;

        const int32_t dme = DaysToMonthEndLocal(row.bucket_id, delta, tz);
        const std::size_t dme_index = 31 + static_cast<std::size_t>(
                                               std::max<int32_t>(
                                                   0,
                                                   std::min<int32_t>(
                                                       dme, shared_config.dme_max)));
        raw[dme_index] = 1.0;

        std::tm local{};
        if (ResolveLocalTime(row.bucket_id, delta, tz, &local) &&
            IsLastWeekdayOfMonthLocal(row.bucket_id, delta, tz)) {
            raw[31 + design.dme_center.size() + static_cast<std::size_t>(local.tm_wday)] = 1.0;
        }

        for (std::size_t i = 0; i < 31; ++i) design.dom_center[i] += raw[i];
        for (std::size_t i = 0; i < design.dme_center.size(); ++i) {
            design.dme_center[i] += raw[31 + i];
        }
        for (std::size_t i = 0; i < 7; ++i) {
            design.lwd_center[i] += raw[31 + design.dme_center.size() + i];
        }

        raw_rows.push_back(std::move(raw));
    }

    for (double& center : design.dom_center) center /= static_cast<double>(rows.size());
    for (double& center : design.dme_center) center /= static_cast<double>(rows.size());
    for (double& center : design.lwd_center) center /= static_cast<double>(rows.size());

    for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
        const auto& row = rows[row_index];
        const double core_mu = EvaluateCore(core_block, row.bucket_id, train_start, delta, tz);
        design.y_target.push_back(row.x - core_mu);
        design.sample_weight.push_back(row.weight);

        std::vector<double>& raw = raw_rows[row_index];
        for (std::size_t i = 0; i < 31; ++i) {
            design.x_matrix.push_back(raw[i] - design.dom_center[i]);
        }
        for (std::size_t i = 0; i < design.dme_center.size(); ++i) {
            design.x_matrix.push_back(raw[31 + i] - design.dme_center[i]);
        }
        for (std::size_t i = 0; i < 7; ++i) {
            design.x_matrix.push_back(raw[31 + design.dme_center.size() + i] - design.lwd_center[i]);
        }
    }

    return design;
}

void ApplyMonthPosBeta(const SharedProfileConfig& shared_config,
                       const MonthPosDesign& design,
                       const std::vector<double>& beta,
                       MonthPosBlock* block) {
    if (!block) return;
    block->enabled = true;
    block->dme_coeff.assign(shared_config.dme_max + 1, 0.0);
    block->dme_center = design.dme_center;
    block->dom_center = design.dom_center;
    block->lwd_center = design.lwd_center;

    std::size_t index = 0;
    for (std::size_t i = 0; i < block->dom_coeff.size() && index < beta.size(); ++i) {
        block->dom_coeff[i] = beta[index++];
    }
    for (std::size_t i = 0; i < block->dme_coeff.size() && index < beta.size(); ++i) {
        block->dme_coeff[i] = beta[index++];
    }
    for (std::size_t i = 0; i < block->lwd_coeff.size() && index < beta.size(); ++i) {
        block->lwd_coeff[i] = beta[index++];
    }
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
            const double indicator = static_cast<int32_t>(i + 1) == dom ? 1.0 : 0.0;
            value += block.dom_coeff[i] * (indicator - block.dom_center[i]);
        }
    }

    const int32_t dme = DaysToMonthEndLocal(bucket_id, delta, tz);
    const int32_t clipped_dme =
        std::max<int32_t>(0, std::min<int32_t>(dme, static_cast<int32_t>(block.dme_coeff.size() - 1)));
    for (std::size_t i = 0; i < block.dme_coeff.size(); ++i) {
        const double indicator = static_cast<int32_t>(i) == clipped_dme ? 1.0 : 0.0;
        value += block.dme_coeff[i] * (indicator - block.dme_center[i]);
    }

    std::tm local{};
    const bool is_last_weekday =
        ResolveLocalTime(bucket_id, delta, tz, &local) &&
        IsLastWeekdayOfMonthLocal(bucket_id, delta, tz);
    for (std::size_t i = 0; i < block.lwd_coeff.size(); ++i) {
        const double indicator =
            (is_last_weekday && static_cast<int32_t>(i) == local.tm_wday) ? 1.0 : 0.0;
        value += block.lwd_coeff[i] * (indicator - block.lwd_center[i]);
    }

    return value;
}

void BuildEventTaskSpec(const ValueFormalTrainInput& input,
                        BaselineTaskSpec* out_task_spec) {
    if (!out_task_spec) return;
    if (input.task_spec) {
        *out_task_spec = *input.task_spec;
        return;
    }
    out_task_spec->feature = "";
    out_task_spec->key = "";
    out_task_spec->delta = input.delta;
    out_task_spec->tz = input.tz;
}

bool BuildEventFitSpec(const ValueFormalTrainInput& input,
                       const std::vector<ValueTrainRow>& rows,
                       const CoreBlock& core_block,
                       const MonthPosBlock& monthpos_block,
                       int64_t train_start,
                       int64_t delta,
                       const std::string& tz,
                       BlockFitSpec* out_spec,
    std::vector<std::string>* out_codes) {
    if (!out_spec || !out_codes) return false;
    if (!input.compiled_event_calendar ||
        input.compiled_event_calendar->enabled_event_codes.empty()) {
        return false;
    }

    BaselineTaskSpec event_task_spec;
    BuildEventTaskSpec(input, &event_task_spec);
    if (event_task_spec.delta <= 0) event_task_spec.delta = delta;
    if (event_task_spec.tz.empty()) event_task_spec.tz = tz;

    BlockFitSpec spec;
    spec.block_name = "event";
    spec.row_count = rows.size();
    spec.col_count = input.compiled_event_calendar->enabled_event_codes.size();
    spec.y_target.reserve(rows.size());
    spec.sample_weight.reserve(rows.size());
    spec.x_matrix.reserve(rows.size() * spec.col_count);
    spec.ridge_diag.assign(spec.col_count, DefaultSharedProfileConfig().lambda_event);
    spec.col_roles.assign(spec.col_count, BlockColumnRole::kEvent);

    std::vector<double> event_row(spec.col_count, 0.0);
    for (const auto& row : rows) {
        std::fill(event_row.begin(), event_row.end(), 0.0);
        if (BuildEventIndicatorRow(*input.compiled_event_calendar,
                                   event_task_spec,
                                   row.bucket_id,
                                   event_row.data(),
                                   event_row.size()) != error::OK) {
            return false;
        }

        const double residual =
            row.x - EvaluateCore(core_block, row.bucket_id, train_start, delta, tz) -
            EvaluateMonthPos(monthpos_block, row.bucket_id, delta, tz);
        spec.y_target.push_back(residual);
        spec.sample_weight.push_back(row.weight);
        spec.x_matrix.insert(spec.x_matrix.end(), event_row.begin(), event_row.end());
    }

    *out_spec = std::move(spec);
    *out_codes = input.compiled_event_calendar->enabled_event_codes;
    return true;
}

void CollectValueTrainRows(const ValueFormalTrainInput& input,
                           std::vector<ValueTrainRow>* out_rows,
                           std::vector<int64_t>* out_bucket_ids) {
    if (!out_rows || !out_bucket_ids) return;
    out_rows->clear();
    out_bucket_ids->clear();

    for (std::size_t i = 0; i < input.train_count; ++i) {
        const auto& point = input.replay->points[i];
        if (input.profile->is_t1b && point.sample_count < input.profile->n_train_min) continue;

        ValueTrainRow row;
        row.bucket_id = point.bucket_id;
        row.x = TransformValueObservation(*input.profile, point.value);
        if (input.profile->is_t1b) {
            const double rho = std::sqrt(
                1.0 + input.profile->kappa_sample / static_cast<double>(point.sample_count));
            row.weight = 1.0 / (rho * rho);
        }
        out_rows->push_back(row);
        out_bucket_ids->push_back(point.bucket_id);
    }
}

void CollectRatioTrainRows(const RatioFormalTrainInput& input,
                           const RatioPriorConfig& prior,
                           std::vector<RatioTrainRow>* out_rows,
                           std::vector<int64_t>* out_bucket_ids) {
    if (!out_rows || !out_bucket_ids) return;
    out_rows->clear();
    out_bucket_ids->clear();

    for (std::size_t i = 0; i < input.train_count; ++i) {
        const auto& point = input.replay->points[i];
        if (point.denominator < static_cast<double>(input.profile->d_min_train)) continue;

        RatioTrainRow row;
        row.bucket_id = point.bucket_id;
        const double smoothed =
            std::min(1.0 - kT2EpsLogit,
                     std::max(kT2EpsLogit,
                              (point.numerator + prior.alpha0) /
                                  (point.denominator + prior.alpha0 + prior.beta0)));
        row.eta = std::log(smoothed / (1.0 - smoothed));
        row.weight =
            point.denominator /
            (point.denominator + static_cast<double>(input.profile->d_min_train));
        out_rows->push_back(row);
        out_bucket_ids->push_back(point.bucket_id);
    }
}

BlockFitSpec BuildCoreFitSpec(const std::vector<RatioTrainRow>& rows,
                              const SharedProfileConfig& shared_config,
                              int64_t delta,
                              const std::string& tz) {
    BlockFitSpec spec;
    spec.block_name = "core";
    spec.row_count = rows.size();
    spec.col_count =
        2 + static_cast<std::size_t>(shared_config.k_day + shared_config.k_week) * 2;
    spec.y_target.reserve(rows.size());
    spec.x_matrix.reserve(rows.size() * spec.col_count);
    spec.sample_weight.reserve(rows.size());
    spec.ridge_diag.assign(spec.col_count, 0.0);
    spec.col_roles.reserve(spec.col_count);

    spec.col_roles.push_back(BlockColumnRole::kIntercept);
    spec.col_roles.push_back(BlockColumnRole::kTrend);
    for (int32_t i = 0; i < shared_config.k_day; ++i) {
        spec.col_roles.push_back(BlockColumnRole::kDaySin);
        spec.col_roles.push_back(BlockColumnRole::kDayCos);
    }
    for (int32_t i = 0; i < shared_config.k_week; ++i) {
        spec.col_roles.push_back(BlockColumnRole::kWeekSin);
        spec.col_roles.push_back(BlockColumnRole::kWeekCos);
    }
    for (std::size_t i = 2; i < spec.col_count; ++i) {
        spec.ridge_diag[i] = shared_config.lambda_season;
    }

    const int64_t train_start = rows.front().bucket_id;
    for (const auto& row : rows) {
        spec.y_target.push_back(row.eta);
        spec.sample_weight.push_back(row.weight);
        AppendCoreRow(shared_config, row.bucket_id, train_start, delta, tz, &spec.x_matrix);
    }
    return spec;
}

MonthPosDesign BuildMonthPosDesign(const std::vector<RatioTrainRow>& rows,
                                   const SharedProfileConfig& shared_config,
                                   const CoreBlock& core_block,
                                   int64_t train_start,
                                   int64_t delta,
                                   const std::string& tz) {
    MonthPosDesign design;
    design.row_count = rows.size();
    design.col_count = 31 + static_cast<std::size_t>(shared_config.dme_max + 1) + 7;
    design.dme_center.assign(shared_config.dme_max + 1, 0.0);
    design.ridge_diag.assign(design.col_count, 0.0);
    design.y_target.reserve(rows.size());
    design.sample_weight.reserve(rows.size());
    design.x_matrix.reserve(rows.size() * design.col_count);

    for (std::size_t i = 0; i < 31; ++i) {
        design.ridge_diag[i] = shared_config.lambda_dom;
    }
    for (std::size_t i = 31; i < 31 + design.dme_center.size(); ++i) {
        design.ridge_diag[i] = shared_config.lambda_dme;
    }
    for (std::size_t i = 31 + design.dme_center.size(); i < design.col_count; ++i) {
        design.ridge_diag[i] = shared_config.lambda_lwd;
    }

    std::vector<std::vector<double>> raw_rows;
    raw_rows.reserve(rows.size());
    for (const auto& row : rows) {
        std::vector<double> raw(design.col_count, 0.0);
        const int32_t dom = DayOfMonthLocal(row.bucket_id, delta, tz);
        if (dom >= 1 && dom <= 31) raw[static_cast<std::size_t>(dom - 1)] = 1.0;

        const int32_t dme = DaysToMonthEndLocal(row.bucket_id, delta, tz);
        const std::size_t dme_index = 31 + static_cast<std::size_t>(
                                               std::max<int32_t>(
                                                   0,
                                                   std::min<int32_t>(
                                                       dme, shared_config.dme_max)));
        raw[dme_index] = 1.0;

        std::tm local{};
        if (ResolveLocalTime(row.bucket_id, delta, tz, &local) &&
            IsLastWeekdayOfMonthLocal(row.bucket_id, delta, tz)) {
            raw[31 + design.dme_center.size() + static_cast<std::size_t>(local.tm_wday)] = 1.0;
        }

        for (std::size_t i = 0; i < 31; ++i) design.dom_center[i] += raw[i];
        for (std::size_t i = 0; i < design.dme_center.size(); ++i) {
            design.dme_center[i] += raw[31 + i];
        }
        for (std::size_t i = 0; i < 7; ++i) {
            design.lwd_center[i] += raw[31 + design.dme_center.size() + i];
        }

        raw_rows.push_back(std::move(raw));
    }

    for (double& center : design.dom_center) center /= static_cast<double>(rows.size());
    for (double& center : design.dme_center) center /= static_cast<double>(rows.size());
    for (double& center : design.lwd_center) center /= static_cast<double>(rows.size());

    for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
        const auto& row = rows[row_index];
        const double core_mu = EvaluateCore(core_block, row.bucket_id, train_start, delta, tz);
        design.y_target.push_back(row.eta - core_mu);
        design.sample_weight.push_back(row.weight);

        std::vector<double>& raw = raw_rows[row_index];
        for (std::size_t i = 0; i < 31; ++i) {
            design.x_matrix.push_back(raw[i] - design.dom_center[i]);
        }
        for (std::size_t i = 0; i < design.dme_center.size(); ++i) {
            design.x_matrix.push_back(raw[31 + i] - design.dme_center[i]);
        }
        for (std::size_t i = 0; i < 7; ++i) {
            design.x_matrix.push_back(raw[31 + design.dme_center.size() + i] - design.lwd_center[i]);
        }
    }

    return design;
}

void BuildEventTaskSpec(const RatioFormalTrainInput& input,
                        BaselineTaskSpec* out_task_spec) {
    if (!out_task_spec) return;
    if (input.task_spec) {
        *out_task_spec = *input.task_spec;
        return;
    }
    out_task_spec->feature = "";
    out_task_spec->key = "";
    out_task_spec->delta = input.delta;
    out_task_spec->tz = input.tz;
}

bool BuildEventFitSpec(const RatioFormalTrainInput& input,
                       const std::vector<RatioTrainRow>& rows,
                       const CoreBlock& core_block,
                       const MonthPosBlock& monthpos_block,
                       int64_t train_start,
                       int64_t delta,
                       const std::string& tz,
                       BlockFitSpec* out_spec,
    std::vector<std::string>* out_codes) {
    if (!out_spec || !out_codes) return false;
    if (!input.compiled_event_calendar ||
        input.compiled_event_calendar->enabled_event_codes.empty()) {
        return false;
    }

    BaselineTaskSpec event_task_spec;
    BuildEventTaskSpec(input, &event_task_spec);
    if (event_task_spec.delta <= 0) event_task_spec.delta = delta;
    if (event_task_spec.tz.empty()) event_task_spec.tz = tz;

    BlockFitSpec spec;
    spec.block_name = "event";
    spec.row_count = rows.size();
    spec.col_count = input.compiled_event_calendar->enabled_event_codes.size();
    spec.y_target.reserve(rows.size());
    spec.sample_weight.reserve(rows.size());
    spec.x_matrix.reserve(rows.size() * spec.col_count);
    spec.ridge_diag.assign(spec.col_count, DefaultSharedProfileConfig().lambda_event);
    spec.col_roles.assign(spec.col_count, BlockColumnRole::kEvent);

    std::vector<double> event_row(spec.col_count, 0.0);
    for (const auto& row : rows) {
        std::fill(event_row.begin(), event_row.end(), 0.0);
        if (BuildEventIndicatorRow(*input.compiled_event_calendar,
                                   event_task_spec,
                                   row.bucket_id,
                                   event_row.data(),
                                   event_row.size()) != error::OK) {
            return false;
        }

        const double residual =
            row.eta - EvaluateCore(core_block, row.bucket_id, train_start, delta, tz) -
            EvaluateMonthPos(monthpos_block, row.bucket_id, delta, tz);
        spec.y_target.push_back(residual);
        spec.sample_weight.push_back(row.weight);
        spec.x_matrix.insert(spec.x_matrix.end(), event_row.begin(), event_row.end());
    }

    *out_spec = std::move(spec);
    *out_codes = input.compiled_event_calendar->enabled_event_codes;
    return true;
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
    if (!SolverBackend::IsAvailable()) {
        return out->failure = FormalTrainFailureCode::kSolverUnavailable;
    }

    const SharedProfileConfig shared_config = DefaultSharedProfileConfig();
    const int64_t delta = ResolveDelta(input);
    const std::string tz = ResolveTimezone(input);

    std::vector<ValueTrainRow> rows;
    std::vector<int64_t> bucket_ids;
    CollectValueTrainRows(input, &rows, &bucket_ids);
    if (rows.size() < 2) {
        return out->failure = FormalTrainFailureCode::kInsufficientTrainData;
    }

    auto model = std::make_shared<ValueFormalModel>();
    model->metadata.kind = FormalModelKind::kValueBaseline;
    FillCommonMetadata(
        input.model_version,
        input.holdout_count,
        input.train_window,
        input.compiled_event_calendar,
        model.get());
    model->transform_name = input.profile->transform_name;
    model->solver_name = DefaultBlockSolverConfig().solver_name;
    model->fit_strategy = "stage_fit";
    model->delta = delta;
    model->tz = tz;
    model->feature_profile = input.profile->feature_profile;
    model->train_start = rows.front().bucket_id;
    model->train_end = rows.back().bucket_id;

    const TrainingCoverageStats coverage_stats =
        BuildTrainingCoverageStats(bucket_ids, delta, tz);
    const ReadinessState train_readiness =
        BuildTrainReadiness(coverage_stats, shared_config);

    const BlockFitSpec core_spec =
        BuildCoreFitSpec(rows, shared_config, delta, tz);
    FitBlockResult core_fit;
    if (SolverBackend::FitWeightedHuberRidgeBlock(
            core_spec, DefaultBlockSolverConfig(), &core_fit) != error::OK ||
        core_fit.beta.empty()) {
        return out->failure = FormalTrainFailureCode::kTrainFailed;
    }
    ApplyCoreBeta(shared_config, core_fit.beta, &model->core_block);
    model->fit_summary.push_back(BuildDigest(
        "core",
        BlockFitStatusName(core_fit.status),
        static_cast<uint64_t>(rows.size()),
        core_fit.objective,
        core_fit.condition_est));

    bool monthpos_applied = false;
    if (train_readiness.monthpos_enabled) {
        const MonthPosDesign month_design =
            BuildMonthPosDesign(rows,
                                shared_config,
                                model->core_block,
                                model->train_start,
                                delta,
                                tz);
        BlockFitSpec month_spec;
        month_spec.block_name = "monthpos";
        month_spec.row_count = month_design.row_count;
        month_spec.col_count = month_design.col_count;
        month_spec.y_target = month_design.y_target;
        month_spec.x_matrix = month_design.x_matrix;
        month_spec.sample_weight = month_design.sample_weight;
        month_spec.ridge_diag = month_design.ridge_diag;
        month_spec.col_roles.assign(month_design.col_count, BlockColumnRole::kMonthDom);
        for (std::size_t i = 31; i < 31 + month_design.dme_center.size(); ++i) {
            month_spec.col_roles[i] = BlockColumnRole::kMonthDme;
        }
        for (std::size_t i = 31 + month_design.dme_center.size(); i < month_design.col_count; ++i) {
            month_spec.col_roles[i] = BlockColumnRole::kMonthLwd;
        }

        FitBlockResult month_fit;
        if (SolverBackend::FitWeightedHuberRidgeBlock(
                month_spec, DefaultBlockSolverConfig(), &month_fit) == error::OK &&
            !month_fit.beta.empty()) {
            ApplyMonthPosBeta(shared_config, month_design, month_fit.beta, &model->monthpos_block);
            monthpos_applied = true;
            model->fit_summary.push_back(BuildDigest(
                "monthpos",
                BlockFitStatusName(month_fit.status),
                static_cast<uint64_t>(rows.size()),
                month_fit.objective,
                month_fit.condition_est));
        } else {
            model->monthpos_block.enabled = false;
            model->fit_summary.push_back(BuildDigest("monthpos", "degraded", 0, 0.0, 0.0));
        }
    } else {
        model->monthpos_block.enabled = false;
        model->monthpos_block.dme_coeff.assign(shared_config.dme_max + 1, 0.0);
        model->monthpos_block.dme_center.assign(shared_config.dme_max + 1, 0.0);
        model->fit_summary.push_back(BuildDigest("monthpos", "skipped", 0, 0.0, 0.0));
    }

    BlockFitSpec event_spec;
    std::vector<std::string> event_codes;
    if (BuildEventFitSpec(input,
                          rows,
                          model->core_block,
                          model->monthpos_block,
                          model->train_start,
                          delta,
                          tz,
                          &event_spec,
                          &event_codes)) {
        FitBlockResult event_fit;
        if (SolverBackend::FitWeightedHuberRidgeBlock(
                event_spec, DefaultBlockSolverConfig(), &event_fit) == error::OK &&
            !event_fit.beta.empty()) {
            model->event_block.enabled = true;
            model->event_block.calendar_id = input.compiled_event_calendar->calendar_id;
            model->event_block.calendar_version = input.compiled_event_calendar->calendar_version;
            model->event_block.active_event_codes = std::move(event_codes);
            model->event_block.coeff = std::move(event_fit.beta);
            model->fit_summary.push_back(BuildDigest(
                "event",
                BlockFitStatusName(event_fit.status),
                static_cast<uint64_t>(rows.size()),
                event_fit.objective,
                event_fit.condition_est));
        } else {
            model->fit_summary.push_back(BuildDigest("event", "degraded", 0, 0.0, 0.0));
        }
    } else {
        model->fit_summary.push_back(BuildDigest("event", "skipped", 0, 0.0, 0.0));
    }

    std::vector<double> residual;
    residual.reserve(rows.size());
    for (const auto& row : rows) {
        double mu = EvaluateCore(model->core_block, row.bucket_id, model->train_start, delta, tz);
        mu += EvaluateMonthPos(model->monthpos_block, row.bucket_id, delta, tz);
        if (model->event_block.enabled && input.task_spec && input.compiled_event_calendar) {
            std::vector<double> indicator(model->event_block.coeff.size(), 0.0);
            if (BuildEventIndicatorRow(*input.compiled_event_calendar,
                                       *input.task_spec,
                                       row.bucket_id,
                                       indicator.data(),
                                       indicator.size()) == error::OK) {
                for (std::size_t i = 0; i < indicator.size(); ++i) {
                    mu += indicator[i] * model->event_block.coeff[i];
                }
            }
        }
        residual.push_back(row.x - mu);
    }
    model->sigma_ref = EstimateSigmaMAD(residual);
    model->readiness = monthpos_applied ? ModelReadiness::kMonthposReady
                                        : ModelReadiness::kCoreNoMonthReady;
    model->confidence_base_at_train = train_readiness.confidence_base;

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

    const SharedProfileConfig shared_config = DefaultSharedProfileConfig();
    const int64_t delta = ResolveDelta(input);
    const std::string tz = ResolveTimezone(input);

    double numerator_sum = 0.0;
    double denominator_sum = 0.0;
    for (std::size_t i = 0; i < input.train_count; ++i) {
        const auto& point = input.replay->points[i];
        if (point.denominator < static_cast<double>(input.profile->d_min_train)) continue;
        numerator_sum += point.numerator;
        denominator_sum += point.denominator;
    }
    T2ProfileConfig profile_config;
    if (!GetT2ProfileConfig(input.profile->feature_profile, &profile_config)) {
        return out->failure = FormalTrainFailureCode::kTrainFailed;
    }
    const RatioPriorConfig prior = ComputeRatioPrior(profile_config, numerator_sum, denominator_sum);

    std::vector<RatioTrainRow> rows;
    std::vector<int64_t> bucket_ids;
    CollectRatioTrainRows(input, prior, &rows, &bucket_ids);
    if (rows.size() < 2) {
        return out->failure = FormalTrainFailureCode::kInsufficientTrainData;
    }

    auto model = std::make_shared<RatioFormalModel>();
    model->metadata.kind = FormalModelKind::kRatioBaseline;
    FillCommonMetadata(
        input.model_version,
        input.holdout_count,
        input.train_window,
        input.compiled_event_calendar,
        model.get());
    model->transform_name = "logit";
    model->solver_name = DefaultBlockSolverConfig().solver_name;
    model->fit_strategy = "stage_fit";
    model->m0 = prior.m0;
    model->alpha0 = prior.alpha0;
    model->beta0 = prior.beta0;
    model->delta = delta;
    model->tz = tz;
    model->feature_profile = input.profile->feature_profile;
    model->train_start = rows.front().bucket_id;
    model->train_end = rows.back().bucket_id;

    const TrainingCoverageStats coverage_stats =
        BuildTrainingCoverageStats(bucket_ids, delta, tz);
    const ReadinessState train_readiness =
        BuildTrainReadiness(coverage_stats, shared_config);

    const BlockFitSpec core_spec =
        BuildCoreFitSpec(rows, shared_config, delta, tz);
    FitBlockResult core_fit;
    if (SolverBackend::FitWeightedHuberRidgeBlock(
            core_spec, DefaultBlockSolverConfig(), &core_fit) != error::OK ||
        core_fit.beta.empty()) {
        return out->failure = FormalTrainFailureCode::kTrainFailed;
    }
    ApplyCoreBeta(shared_config, core_fit.beta, &model->core_block);
    model->fit_summary.push_back(BuildDigest(
        "core",
        BlockFitStatusName(core_fit.status),
        static_cast<uint64_t>(rows.size()),
        core_fit.objective,
        core_fit.condition_est));

    bool monthpos_applied = false;
    if (train_readiness.monthpos_enabled) {
        const MonthPosDesign month_design =
            BuildMonthPosDesign(rows,
                                shared_config,
                                model->core_block,
                                model->train_start,
                                delta,
                                tz);
        BlockFitSpec month_spec;
        month_spec.block_name = "monthpos";
        month_spec.row_count = month_design.row_count;
        month_spec.col_count = month_design.col_count;
        month_spec.y_target = month_design.y_target;
        month_spec.x_matrix = month_design.x_matrix;
        month_spec.sample_weight = month_design.sample_weight;
        month_spec.ridge_diag = month_design.ridge_diag;
        month_spec.col_roles.assign(month_design.col_count, BlockColumnRole::kMonthDom);
        for (std::size_t i = 31; i < 31 + month_design.dme_center.size(); ++i) {
            month_spec.col_roles[i] = BlockColumnRole::kMonthDme;
        }
        for (std::size_t i = 31 + month_design.dme_center.size(); i < month_design.col_count; ++i) {
            month_spec.col_roles[i] = BlockColumnRole::kMonthLwd;
        }

        FitBlockResult month_fit;
        if (SolverBackend::FitWeightedHuberRidgeBlock(
                month_spec, DefaultBlockSolverConfig(), &month_fit) == error::OK &&
            !month_fit.beta.empty()) {
            ApplyMonthPosBeta(shared_config, month_design, month_fit.beta, &model->monthpos_block);
            monthpos_applied = true;
            model->fit_summary.push_back(BuildDigest(
                "monthpos",
                BlockFitStatusName(month_fit.status),
                static_cast<uint64_t>(rows.size()),
                month_fit.objective,
                month_fit.condition_est));
        } else {
            model->monthpos_block.enabled = false;
            model->fit_summary.push_back(BuildDigest("monthpos", "degraded", 0, 0.0, 0.0));
        }
    } else {
        model->monthpos_block.enabled = false;
        model->monthpos_block.dme_coeff.assign(shared_config.dme_max + 1, 0.0);
        model->monthpos_block.dme_center.assign(shared_config.dme_max + 1, 0.0);
        model->fit_summary.push_back(BuildDigest("monthpos", "skipped", 0, 0.0, 0.0));
    }

    BlockFitSpec event_spec;
    std::vector<std::string> event_codes;
    if (BuildEventFitSpec(input,
                          rows,
                          model->core_block,
                          model->monthpos_block,
                          model->train_start,
                          delta,
                          tz,
                          &event_spec,
                          &event_codes)) {
        FitBlockResult event_fit;
        if (SolverBackend::FitWeightedHuberRidgeBlock(
                event_spec, DefaultBlockSolverConfig(), &event_fit) == error::OK &&
            !event_fit.beta.empty()) {
            model->event_block.enabled = true;
            model->event_block.calendar_id = input.compiled_event_calendar->calendar_id;
            model->event_block.calendar_version = input.compiled_event_calendar->calendar_version;
            model->event_block.active_event_codes = std::move(event_codes);
            model->event_block.coeff = std::move(event_fit.beta);
            model->fit_summary.push_back(BuildDigest(
                "event",
                BlockFitStatusName(event_fit.status),
                static_cast<uint64_t>(rows.size()),
                event_fit.objective,
                event_fit.condition_est));
        } else {
            model->fit_summary.push_back(BuildDigest("event", "degraded", 0, 0.0, 0.0));
        }
    } else {
        model->fit_summary.push_back(BuildDigest("event", "skipped", 0, 0.0, 0.0));
    }

    model->readiness = monthpos_applied ? ModelReadiness::kMonthposReady
                                        : ModelReadiness::kCoreNoMonthReady;
    model->confidence_base_at_train = train_readiness.confidence_base;

    out->failure = FormalTrainFailureCode::kNone;
    out->model = std::move(model);
    return out->failure;
}

}  // namespace baseline
}  // namespace flowsql
