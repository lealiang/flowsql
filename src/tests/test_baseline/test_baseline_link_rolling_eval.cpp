/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <common/loader.hpp>
#include <framework/interfaces/ibaseline_service.h>

using namespace flowsql;

namespace {

constexpr int64_t kBucketSeconds = 60;
constexpr int64_t kDayMinutes = 24 * 60;
constexpr int64_t kWeekMinutes = 7 * kDayMinutes;
constexpr int64_t kShanghaiUtcOffsetSeconds = 8 * 60 * 60;
constexpr double kConfidenceLevel = 0.95;
constexpr double kBootstrapBandZ = 1.96;
constexpr double kRollingBandZ = 3.0;
constexpr int32_t kDefaultDailyHarmonicOrder = 6;
constexpr int32_t kDefaultWeeklyHarmonicOrder = 3;
constexpr const char* kSeriesKey = "link-traffic-bps";

struct LoadedBaselineService {
    PluginLoader* loader = nullptr;
    IBaselineService* service = nullptr;

    ~LoadedBaselineService() {
        if (!loader) return;
        loader->StopAll();
        loader->Unload();
    }
};

struct LinkPoint {
    std::string timestamp;
    int64_t bucket_id = 0;
    double mbps = 0.0;
};

struct EvalSummary {
    uint64_t count = 0;
    uint64_t inside_band_count = 0;
    uint64_t abs_z_gt_3_count = 0;
    uint64_t abs_z_gt_5_count = 0;
    double coverage_ratio = 0.0;
    double mean_abs_z = 0.0;
    double max_abs_z = 0.0;
    double rmse = 0.0;
    double mape = 0.0;
    double mean_band_width = 0.0;
};

struct TransitionCheckpoint {
    int64_t minute_offset = 0;
    std::string timestamp;
    double actual = 0.0;
    double baseline_mu = 0.0;
    double abs_z = 0.0;
    double drift_evidence = 0.0;
    double adapt_boost = 0.0;
    double update_weight = 0.0;
};

struct BandCalibration {
    uint64_t count = 0;
    double p95_abs_z = 0.0;
    double multiplier = 1.0;
};

struct EvalAccumulator {
    uint64_t count = 0;
    uint64_t inside_band_count = 0;
    uint64_t abs_z_gt_3_count = 0;
    uint64_t abs_z_gt_5_count = 0;
    double sum_abs_z = 0.0;
    double max_abs_z = 0.0;
    double sum_square_error = 0.0;
    double sum_abs_percentage_error = 0.0;
    double sum_band_width = 0.0;

    void Add(double actual,
             double baseline_mu,
             double baseline_lower,
             double baseline_upper,
             double band_z) {
        const double z = DirectionalZ(actual,
                                      baseline_mu,
                                      baseline_lower,
                                      baseline_upper,
                                      band_z);
        const double abs_z = std::fabs(z);
        const double error = actual - baseline_mu;
        if (actual >= baseline_lower && actual <= baseline_upper) ++inside_band_count;
        if (abs_z > 3.0) ++abs_z_gt_3_count;
        if (abs_z > 5.0) ++abs_z_gt_5_count;
        sum_abs_z += abs_z;
        max_abs_z = std::max(max_abs_z, abs_z);
        sum_square_error += error * error;
        sum_band_width += std::max(0.0, baseline_upper - baseline_lower);
        if (std::fabs(actual) > 1.0e-9) {
            sum_abs_percentage_error += std::fabs(error / actual);
        }
        ++count;
    }

    EvalSummary Finish() const {
        EvalSummary summary;
        summary.count = count;
        summary.inside_band_count = inside_band_count;
        summary.abs_z_gt_3_count = abs_z_gt_3_count;
        summary.abs_z_gt_5_count = abs_z_gt_5_count;
        summary.max_abs_z = max_abs_z;
        if (count > 0) {
            const double n = static_cast<double>(count);
            summary.coverage_ratio = static_cast<double>(inside_band_count) / n;
            summary.mean_abs_z = sum_abs_z / n;
            summary.rmse = std::sqrt(sum_square_error / n);
            summary.mape = sum_abs_percentage_error / n;
            summary.mean_band_width = sum_band_width / n;
        }
        return summary;
    }

    static double DirectionalZ(double actual,
                               double baseline_mu,
                               double baseline_lower,
                               double baseline_upper,
                               double band_z) {
        const double safe_z = band_z > 1.0e-9 ? band_z : 1.0;
        const double upper_sigma = (baseline_upper - baseline_mu) / safe_z;
        const double lower_sigma = (baseline_mu - baseline_lower) / safe_z;
        const double fallback_sigma =
            std::max((baseline_upper - baseline_lower) / (2.0 * safe_z), 1.0e-9);
        double sigma = fallback_sigma;
        if (actual >= baseline_mu && upper_sigma > 1.0e-9) {
            sigma = upper_sigma;
        } else if (actual < baseline_mu && lower_sigma > 1.0e-9) {
            sigma = lower_sigma;
        }
        return (actual - baseline_mu) / sigma;
    }
};

bool ParseTimestampMinute(const std::string& text, int64_t* out_bucket_id) {
    if (!out_bucket_id) return false;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    if (std::sscanf(text.c_str(), "%d/%d/%d %d:%d", &year, &month, &day, &hour, &minute) != 5) {
        return false;
    }

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = 0;
    tm.tm_isdst = 0;
    const std::time_t utc_if_local_was_utc = timegm(&tm);
    if (utc_if_local_was_utc == static_cast<std::time_t>(-1)) return false;
    *out_bucket_id =
        (static_cast<int64_t>(utc_if_local_was_utc) - kShanghaiUtcOffsetSeconds) /
        kBucketSeconds;
    return true;
}

bool LoadCsv(const std::string& path, std::vector<LinkPoint>* out_points) {
    if (!out_points) return false;
    out_points->clear();

    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "failed to open csv: " << path << "\n";
        return false;
    }

    std::string line;
    if (!std::getline(file, line)) return false;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        const std::size_t comma = line.find(',');
        if (comma == std::string::npos) {
            std::cerr << "invalid csv line: " << line << "\n";
            return false;
        }
        LinkPoint point;
        point.timestamp = line.substr(0, comma);
        if (!ParseTimestampMinute(point.timestamp, &point.bucket_id)) {
            std::cerr << "invalid timestamp: " << point.timestamp << "\n";
            return false;
        }
        point.mbps = std::strtod(line.c_str() + comma + 1, nullptr);
        if (!std::isfinite(point.mbps)) {
            std::cerr << "invalid Mbps value: " << line << "\n";
            return false;
        }
        out_points->push_back(std::move(point));
    }
    return !out_points->empty();
}

bool WriteTextFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "failed to open output: " << path << "\n";
        return false;
    }
    file << content;
    return file.good();
}

std::string BuildRuntimeConfigYaml(int32_t daily_harmonic_order,
                                   int32_t weekly_harmonic_order) {
    std::ostringstream out;
    out << "baseline:\n";
    out << "  parser:\n";
    out << "    tz_default: \"Asia/Shanghai\"\n";
    out << "  shared_profile_config:\n";
    out << "    daily_harmonic_order: " << daily_harmonic_order << "\n";
    out << "    weekly_harmonic_order: " << weekly_harmonic_order << "\n";
    out << "    dme_max: 7\n";
    out << "    m_month_enable: 4\n";
    out << "    month_cov_min: 0.8\n";
    out << "    lambda_season: 1.0\n";
    out << "    lambda_dom: 4.0\n";
    out << "    lambda_dme: 2.0\n";
    out << "    lambda_lwd: 1.0\n";
    out << "    lambda_event: 2.0\n";
    out << "  rolling_config:\n";
    out << "    daily_harmonic_order: " << daily_harmonic_order << "\n";
    out << "    weekly_harmonic_order: " << weekly_harmonic_order << "\n";
    out << "    band_z: " << kRollingBandZ << "\n";
    out << "  value_sampled_profiles:\n";
    out << "    cont_core:\n";
    out << "      n_train_min: 50\n";
    out << "      transform_name_override: \"log1p\"\n";
    out << "  ratio_profiles:\n";
    out << "    global:\n";
    out << "      eps_logit: 1.0e-4\n";
    out << "      m_floor: 1.0e-4\n";
    out << "      v_floor: 0.25\n";
    out << "    rate_core:\n";
    out << "      d_min_train: 50\n";
    out << "      s_prior: 2.0\n";
    out << "      phi_over: 1.5\n";
    out << "  solver_constants:\n";
    out << "    solver_name: \"weighted_huber_ridge_irls\"\n";
    out << "    c_huber: 1.5\n";
    out << "    s_min_fit: 1.0e-3\n";
    out << "    max_iter_fit: 15\n";
    out << "    tol_obj_rel: 1.0e-4\n";
    out << "    tol_beta_inf: 1.0e-5\n";
    out << "    cond_max: 1.0e8\n";
    return out.str();
}

LoadedBaselineService LoadBaselineService(const std::filesystem::path& config_path) {
    LoadedBaselineService env;
    env.loader = PluginLoader::Single();

    std::string plugin_dir = get_absolute_process_path();
    std::string plugin_name = "libflowsql_baseline.so";
    const char* relapath[] = {plugin_name.c_str()};
    const std::string option = "config_file=" + config_path.string() + ";strict=true";
    const char* options[] = {option.c_str()};

    const int ret = env.loader->Load(plugin_dir.c_str(), relapath, options, 1);
    assert(ret == 0);
    assert(env.loader->StartAll() == 0);

    env.service = static_cast<IBaselineService*>(env.loader->First(IID_BASELINE_SERVICE));
    assert(env.service != nullptr);
    return env;
}

std::string ValueTaskConfig() {
    return R"({
        "schema_version": 1,
        "task_id": "link_bps_rolling_eval",
        "task_name": "link bps rolling eval",
        "task_kind": "value",
        "feature_id": "link_bps_mbps",
        "feature_type": "value_basic",
        "profile": "default",
        "clock_spec": {
            "bucket_seconds": 60,
            "timezone": "Asia/Shanghai"
        },
        "calendar_ref": {
            "calendar_id": "cn-holiday",
            "calendar_version": "2026.1"
        }
    })";
}

std::size_t FindIndexGE(const std::vector<LinkPoint>& points, int64_t bucket_id) {
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (points[i].bucket_id >= bucket_id) return i;
    }
    return points.size();
}

ValueRollingObservation ToObservation(const LinkPoint& point) {
    ValueRollingObservation obs;
    obs.series_key = kSeriesKey;
    obs.bucket_id = point.bucket_id;
    obs.value = point.mbps;
    return obs;
}

void TrainRollingRange(IBaselineValueTask* task,
                       const std::vector<LinkPoint>& points,
                       std::size_t begin,
                       std::size_t end) {
    for (std::size_t i = begin; i < end; ++i) {
        const RollingBaselineResult result =
            task->SubmitObservation(ToObservation(points[i]), RollingSubmitOptions{});
        if (result.status != BaselineStatus::kOk) {
            std::cerr << "rolling train failed at " << points[i].timestamp
                      << ", status=" << static_cast<int>(result.status) << "\n";
            std::abort();
        }
    }
}

void AppendMetricsJson(std::ostringstream* out,
                       const EvalSummary& summary,
                       const std::string& indent) {
    *out << indent << "\"count\": " << summary.count << ",\n";
    *out << indent << "\"inside_band_count\": " << summary.inside_band_count << ",\n";
    *out << indent << "\"abs_z_gt_3_count\": " << summary.abs_z_gt_3_count << ",\n";
    *out << indent << "\"abs_z_gt_5_count\": " << summary.abs_z_gt_5_count << ",\n";
    *out << indent << "\"coverage_ratio\": " << std::fixed << std::setprecision(6)
         << summary.coverage_ratio << ",\n";
    *out << indent << "\"mean_abs_z\": " << summary.mean_abs_z << ",\n";
    *out << indent << "\"max_abs_z\": " << summary.max_abs_z << ",\n";
    *out << indent << "\"rmse_mbps\": " << summary.rmse << ",\n";
    *out << indent << "\"mape\": " << summary.mape << ",\n";
    *out << indent << "\"mean_band_width_mbps\": " << summary.mean_band_width << "\n";
}

bool WriteWeek4ComparisonAndTrainRolling(const std::filesystem::path& path,
                                         IBaselineValueTask* task,
                                         const std::vector<LinkPoint>& points,
                                         std::size_t begin,
                                         std::size_t end,
                                         EvalSummary* bootstrap_summary,
                                         EvalSummary* rolling_summary) {
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << "timestamp,bucket_id,actual_mbps,bootstrap_mu,bootstrap_lower,"
            "bootstrap_upper,bootstrap_abs_z,rolling_mu,rolling_lower,rolling_upper,"
            "rolling_abs_z\n";

    BootstrapPredictionOptions bootstrap_options;
    bootstrap_options.confidence_level = kConfidenceLevel;
    EvalAccumulator bootstrap_acc;
    EvalAccumulator rolling_acc;
    for (std::size_t i = begin; i < end; ++i) {
        const LinkPoint& point = points[i];
        const BootstrapPrediction bp =
            task->PredictBootstrap(kSeriesKey, point.bucket_id, bootstrap_options);
        const RollingBaselineResult rr =
            task->SubmitObservation(ToObservation(point), RollingSubmitOptions{});
        if (bp.status != BaselineStatus::kOk || rr.status != BaselineStatus::kOk) {
            std::cerr << "week4 prediction failed at " << point.timestamp << "\n";
            return false;
        }
        bootstrap_acc.Add(point.mbps,
                          bp.baseline_mu,
                          bp.baseline_lower,
                          bp.baseline_upper,
                          kBootstrapBandZ);
        rolling_acc.Add(point.mbps,
                        rr.baseline_mu,
                        rr.baseline_lower,
                        rr.baseline_upper,
                        kRollingBandZ);
        const double bz = EvalAccumulator::DirectionalZ(point.mbps,
                                                        bp.baseline_mu,
                                                        bp.baseline_lower,
                                                        bp.baseline_upper,
                                                        kBootstrapBandZ);
        const double rz = EvalAccumulator::DirectionalZ(point.mbps,
                                                        rr.baseline_mu,
                                                        rr.baseline_lower,
                                                        rr.baseline_upper,
                                                        kRollingBandZ);
        file << point.timestamp << ',' << point.bucket_id << ',' << std::fixed
             << std::setprecision(6) << point.mbps << ',' << bp.baseline_mu << ','
             << bp.baseline_lower << ',' << bp.baseline_upper << ',' << std::fabs(bz) << ','
             << rr.baseline_mu << ',' << rr.baseline_lower << ',' << rr.baseline_upper << ','
             << std::fabs(rz) << '\n';
    }
    *bootstrap_summary = bootstrap_acc.Finish();
    *rolling_summary = rolling_acc.Finish();
    return file.good();
}

bool WriteTransitionLearning(const std::filesystem::path& path,
                             IBaselineValueTask* task,
                             const std::vector<LinkPoint>& points,
                             std::size_t begin,
                             std::size_t end,
                             EvalSummary* summary,
                             std::vector<TransitionCheckpoint>* checkpoints) {
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << "timestamp,bucket_id,minute_offset,actual_mbps,baseline_mu,baseline_lower,"
            "baseline_upper,z_score,abs_z,drift_evidence,adapt_boost,update_weight\n";

    EvalAccumulator acc;
    for (std::size_t i = begin; i < end; ++i) {
        const LinkPoint& point = points[i];
        const RollingBaselineResult result =
            task->SubmitObservation(ToObservation(point), RollingSubmitOptions{});
        if (result.status != BaselineStatus::kOk) {
            std::cerr << "transition update failed at " << point.timestamp
                      << ", status=" << static_cast<int>(result.status) << "\n";
            return false;
        }
        acc.Add(point.mbps,
                result.baseline_mu,
                result.baseline_lower,
                result.baseline_upper,
                3.0);
        const int64_t minute_offset = point.bucket_id - points[begin].bucket_id;
        if (checkpoints &&
            (minute_offset == 0 || minute_offset == 60 || minute_offset == 240 ||
             minute_offset == 720 || minute_offset == 1439)) {
            TransitionCheckpoint checkpoint;
            checkpoint.minute_offset = minute_offset;
            checkpoint.timestamp = point.timestamp;
            checkpoint.actual = point.mbps;
            checkpoint.baseline_mu = result.baseline_mu;
            checkpoint.abs_z = std::fabs(result.z_score);
            checkpoint.drift_evidence = result.drift_evidence;
            checkpoint.adapt_boost = result.adapt_boost;
            checkpoint.update_weight = result.update_weight;
            checkpoints->push_back(std::move(checkpoint));
        }
        file << point.timestamp << ',' << point.bucket_id << ',' << minute_offset << ','
             << std::fixed << std::setprecision(6) << point.mbps << ','
             << result.baseline_mu << ',' << result.baseline_lower << ','
             << result.baseline_upper << ',' << result.z_score << ','
             << std::fabs(result.z_score) << ',' << result.drift_evidence << ','
             << result.adapt_boost << ',' << result.update_weight << '\n';
    }
    *summary = acc.Finish();
    return file.good();
}

bool WriteRollingPredictions(const std::filesystem::path& path,
                             IBaselineValueTask* task,
                             const std::vector<LinkPoint>& points,
                             std::size_t begin,
                             std::size_t end,
                             EvalSummary* summary) {
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << "timestamp,bucket_id,actual_mbps,baseline_mu,baseline_lower,"
            "baseline_upper,band_width,abs_z,in_band\n";

    EvalAccumulator acc;
    for (std::size_t i = begin; i < end; ++i) {
        const LinkPoint& point = points[i];
        const RollingPrediction prediction = task->PredictRolling(kSeriesKey, point.bucket_id);
        if (prediction.status != BaselineStatus::kOk) {
            std::cerr << "rolling prediction failed at " << point.timestamp
                      << ", status=" << static_cast<int>(prediction.status) << "\n";
            return false;
        }
        acc.Add(point.mbps,
                prediction.baseline_mu,
                prediction.baseline_lower,
                prediction.baseline_upper,
                prediction.band_z);
        const double z = EvalAccumulator::DirectionalZ(point.mbps,
                                                       prediction.baseline_mu,
                                                       prediction.baseline_lower,
                                                       prediction.baseline_upper,
                                                       prediction.band_z);
        const bool in_band =
            point.mbps >= prediction.baseline_lower && point.mbps <= prediction.baseline_upper;
        file << point.timestamp << ',' << point.bucket_id << ',' << std::fixed
             << std::setprecision(6) << point.mbps << ',' << prediction.baseline_mu << ','
             << prediction.baseline_lower << ',' << prediction.baseline_upper << ','
             << (prediction.baseline_upper - prediction.baseline_lower) << ','
             << std::fabs(z) << ',' << (in_band ? 1 : 0) << '\n';
    }
    *summary = acc.Finish();
    return file.good();
}

bool WriteAdaptiveForecastPredictions(const std::filesystem::path& path,
                                      IBaselineValueTask* task,
                                      const std::vector<LinkPoint>& points,
                                      std::size_t begin,
                                      std::size_t end,
                                      EvalSummary* summary) {
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << "timestamp,bucket_id,actual_mbps,rolling_mu,bootstrap_mu,adaptive_lower,"
            "adaptive_upper,adaptive_band_width,abs_z,in_band\n";

    BootstrapPredictionOptions bootstrap_options;
    bootstrap_options.confidence_level = kConfidenceLevel;
    EvalAccumulator acc;
    for (std::size_t i = begin; i < end; ++i) {
        const LinkPoint& point = points[i];
        const RollingPrediction rolling = task->PredictRolling(kSeriesKey, point.bucket_id);
        const BootstrapPrediction bootstrap =
            task->PredictBootstrap(kSeriesKey, point.bucket_id, bootstrap_options);
        if (rolling.status != BaselineStatus::kOk ||
            bootstrap.status != BaselineStatus::kOk) {
            std::cerr << "adaptive forecast failed at " << point.timestamp << "\n";
            return false;
        }

        const double bootstrap_half_width =
            std::max(0.0, bootstrap.baseline_upper - bootstrap.baseline_lower) * 0.5;
        const double lower = std::max(0.0, rolling.baseline_mu - bootstrap_half_width);
        const double upper = rolling.baseline_mu + bootstrap_half_width;
        acc.Add(point.mbps, rolling.baseline_mu, lower, upper, kBootstrapBandZ);
        const double z = EvalAccumulator::DirectionalZ(point.mbps,
                                                       rolling.baseline_mu,
                                                       lower,
                                                       upper,
                                                       kBootstrapBandZ);
        const bool in_band = point.mbps >= lower && point.mbps <= upper;
        file << point.timestamp << ',' << point.bucket_id << ',' << std::fixed
             << std::setprecision(6) << point.mbps << ',' << rolling.baseline_mu << ','
             << bootstrap.baseline_mu << ',' << lower << ',' << upper << ','
             << (upper - lower) << ',' << std::fabs(z) << ',' << (in_band ? 1 : 0)
             << '\n';
    }
    *summary = acc.Finish();
    return file.good();
}

double ClampDouble(double value, double lo, double hi) {
    return std::max(lo, std::min(hi, value));
}

double Quantile(std::vector<double> values, double q) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double pos = ClampDouble(q, 0.0, 1.0) *
                       static_cast<double>(values.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(std::floor(pos));
    const std::size_t hi = static_cast<std::size_t>(std::ceil(pos));
    if (lo == hi) return values[lo];
    const double w = pos - static_cast<double>(lo);
    return values[lo] * (1.0 - w) + values[hi] * w;
}

BandCalibration TrainRollingRangeAndCalibrateLevelScaledBand(
    IBaselineValueTask* task,
    const std::vector<LinkPoint>& points,
    std::size_t begin,
    std::size_t end) {
    BootstrapPredictionOptions bootstrap_options;
    bootstrap_options.confidence_level = kConfidenceLevel;
    std::vector<double> abs_z_values;
    abs_z_values.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
        const LinkPoint& point = points[i];
        const BootstrapPrediction bootstrap =
            task->PredictBootstrap(kSeriesKey, point.bucket_id, bootstrap_options);
        const RollingBaselineResult rolling =
            task->SubmitObservation(ToObservation(point), RollingSubmitOptions{});
        if (bootstrap.status != BaselineStatus::kOk ||
            rolling.status != BaselineStatus::kOk) {
            std::cerr << "post-shift calibration failed at " << point.timestamp << "\n";
            std::abort();
        }
        const double bootstrap_half_width =
            std::max(0.0, bootstrap.baseline_upper - bootstrap.baseline_lower) * 0.5;
        const double raw_scale =
            bootstrap.baseline_mu > 1.0e-9 ? rolling.baseline_mu / bootstrap.baseline_mu : 1.0;
        const double scale = ClampDouble(raw_scale, 0.05, 2.0);
        const double half_width = bootstrap_half_width * scale;
        const double lower = std::max(0.0, rolling.baseline_mu - half_width);
        const double upper = rolling.baseline_mu + half_width;
        const double z = EvalAccumulator::DirectionalZ(point.mbps,
                                                       rolling.baseline_mu,
                                                       lower,
                                                       upper,
                                                       kBootstrapBandZ);
        abs_z_values.push_back(std::fabs(z));
    }

    BandCalibration calibration;
    calibration.count = static_cast<uint64_t>(abs_z_values.size());
    calibration.p95_abs_z = Quantile(abs_z_values, 0.95);
    calibration.multiplier = std::max(1.0, calibration.p95_abs_z / kBootstrapBandZ);
    return calibration;
}

bool WriteLevelScaledAdaptiveForecastPredictions(const std::filesystem::path& path,
                                                 IBaselineValueTask* task,
                                                 const std::vector<LinkPoint>& points,
                                                 std::size_t begin,
                                                 std::size_t end,
                                                 double band_multiplier,
                                                 EvalSummary* summary) {
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << "timestamp,bucket_id,actual_mbps,rolling_mu,bootstrap_mu,scale,"
            "scaled_lower,scaled_upper,scaled_band_width,abs_z,in_band\n";

    BootstrapPredictionOptions bootstrap_options;
    bootstrap_options.confidence_level = kConfidenceLevel;
    EvalAccumulator acc;
    for (std::size_t i = begin; i < end; ++i) {
        const LinkPoint& point = points[i];
        const RollingPrediction rolling = task->PredictRolling(kSeriesKey, point.bucket_id);
        const BootstrapPrediction bootstrap =
            task->PredictBootstrap(kSeriesKey, point.bucket_id, bootstrap_options);
        if (rolling.status != BaselineStatus::kOk ||
            bootstrap.status != BaselineStatus::kOk) {
            std::cerr << "level-scaled adaptive forecast failed at "
                      << point.timestamp << "\n";
            return false;
        }

        const double bootstrap_half_width =
            std::max(0.0, bootstrap.baseline_upper - bootstrap.baseline_lower) * 0.5;
        const double raw_scale =
            bootstrap.baseline_mu > 1.0e-9 ? rolling.baseline_mu / bootstrap.baseline_mu : 1.0;
        const double scale = ClampDouble(raw_scale, 0.05, 2.0);
        const double scaled_half_width = bootstrap_half_width * scale * band_multiplier;
        const double lower = std::max(0.0, rolling.baseline_mu - scaled_half_width);
        const double upper = rolling.baseline_mu + scaled_half_width;
        acc.Add(point.mbps, rolling.baseline_mu, lower, upper, kBootstrapBandZ);
        const double z = EvalAccumulator::DirectionalZ(point.mbps,
                                                       rolling.baseline_mu,
                                                       lower,
                                                       upper,
                                                       kBootstrapBandZ);
        const bool in_band = point.mbps >= lower && point.mbps <= upper;
        file << point.timestamp << ',' << point.bucket_id << ',' << std::fixed
             << std::setprecision(6) << point.mbps << ',' << rolling.baseline_mu << ','
             << bootstrap.baseline_mu << ',' << scale << ',' << lower << ',' << upper << ','
             << (upper - lower) << ',' << std::fabs(z) << ',' << (in_band ? 1 : 0)
             << '\n';
    }
    *summary = acc.Finish();
    return file.good();
}

std::string SummaryJson(const EvalSummary& week4_bootstrap,
                        const EvalSummary& week4_rolling,
                        const EvalSummary& transition,
                        const EvalSummary& final7,
                        const EvalSummary& final7_adaptive_forecast,
                        const EvalSummary& final7_level_scaled_forecast,
                        const EvalSummary& final7_calibrated_scaled_forecast,
                        const BandCalibration& band_calibration,
                        const std::vector<LinkPoint>& points,
                        std::size_t bootstrap_begin,
                        std::size_t bootstrap_end,
                        std::size_t week3_begin,
                        std::size_t week3_end,
                        std::size_t week4_begin,
                        std::size_t week4_end,
                        std::size_t transition_begin,
                        std::size_t transition_end,
                        std::size_t post_train_begin,
                        std::size_t post_train_end,
                        std::size_t final_begin,
                        std::size_t final_end,
                        const std::vector<TransitionCheckpoint>& checkpoints,
                        const std::filesystem::path& week4_csv,
                        const std::filesystem::path& transition_csv,
                        const std::filesystem::path& final_csv,
                        const std::filesystem::path& adaptive_csv,
                        const std::filesystem::path& scaled_csv,
                        const std::filesystem::path& calibrated_csv) {
    auto range = [&](std::size_t begin, std::size_t end) {
        std::ostringstream out;
        out << "\"start_time\": \"" << points[begin].timestamp << "\", ";
        out << "\"end_time\": \"" << points[end - 1].timestamp << "\", ";
        out << "\"count\": " << (end - begin);
        return out.str();
    };

    std::ostringstream out;
    out << "{\n";
    out << "  \"schema_version\": 1,\n";
    out << "  \"document_kind\": \"baseline_rolling_eval_summary\",\n";
    out << "  \"csv\": \"src/tests/data/csv/link_data_2_month.csv\",\n";
    out << "  \"feature\": \"link_bps_mbps\",\n";
    out << "  \"unit\": \"Mbps\",\n";
    out << "  \"bucket_seconds\": 60,\n";
    out << "  \"timezone\": \"Asia/Shanghai\",\n";
    out << "  \"windows\": {\n";
    out << "    \"bootstrap_train\": {" << range(bootstrap_begin, bootstrap_end) << "},\n";
    out << "    \"rolling_train_week3\": {" << range(week3_begin, week3_end) << "},\n";
    out << "    \"week4_compare_walk_forward\": {"
        << range(week4_begin, week4_end) << "},\n";
    out << "    \"transition_learning\": {"
        << range(transition_begin, transition_end) << "},\n";
    out << "    \"post_shift_train_4d\": {"
        << range(post_train_begin, post_train_end) << "},\n";
    out << "    \"final7_predict\": {" << range(final_begin, final_end) << "}\n";
    out << "  },\n";
    out << "  \"metrics\": {\n";
    out << "    \"week4_bootstrap\": {\n";
    AppendMetricsJson(&out, week4_bootstrap, "      ");
    out << "    },\n";
    out << "    \"week4_rolling\": {\n";
    AppendMetricsJson(&out, week4_rolling, "      ");
    out << "    },\n";
    out << "    \"transition_learning\": {\n";
    AppendMetricsJson(&out, transition, "      ");
    out << "    },\n";
    out << "    \"final7_rolling\": {\n";
    AppendMetricsJson(&out, final7, "      ");
    out << "    },\n";
    out << "    \"final7_adaptive_forecast\": {\n";
    AppendMetricsJson(&out, final7_adaptive_forecast, "      ");
    out << "    },\n";
    out << "    \"final7_level_scaled_forecast\": {\n";
    AppendMetricsJson(&out, final7_level_scaled_forecast, "      ");
    out << "    },\n";
    out << "    \"final7_calibrated_scaled_forecast\": {\n";
    AppendMetricsJson(&out, final7_calibrated_scaled_forecast, "      ");
    out << "    }\n";
    out << "  },\n";
    out << "  \"band_calibration\": {\n";
    out << "    \"source_window\": \"post_shift_train_4d\",\n";
    out << "    \"count\": " << band_calibration.count << ",\n";
    out << "    \"p95_abs_z_before_multiplier\": " << band_calibration.p95_abs_z << ",\n";
    out << "    \"multiplier\": " << band_calibration.multiplier << "\n";
    out << "  },\n";
    out << "  \"transition_checkpoints\": [\n";
    for (std::size_t i = 0; i < checkpoints.size(); ++i) {
        const TransitionCheckpoint& item = checkpoints[i];
        out << "    {\n";
        out << "      \"minute_offset\": " << item.minute_offset << ",\n";
        out << "      \"timestamp\": \"" << item.timestamp << "\",\n";
        out << "      \"actual_mbps\": " << item.actual << ",\n";
        out << "      \"baseline_mu_mbps\": " << item.baseline_mu << ",\n";
        out << "      \"abs_z\": " << item.abs_z << ",\n";
        out << "      \"drift_evidence\": " << item.drift_evidence << ",\n";
        out << "      \"adapt_boost\": " << item.adapt_boost << ",\n";
        out << "      \"update_weight\": " << item.update_weight << "\n";
        out << "    }" << (i + 1 < checkpoints.size() ? "," : "") << "\n";
    }
    out << "  ],\n";
    out << "  \"outputs\": {\n";
    out << "    \"week4_comparison_csv\": \"" << week4_csv.string() << "\",\n";
    out << "    \"transition_learning_csv\": \"" << transition_csv.string() << "\",\n";
    out << "    \"final7_rolling_csv\": \"" << final_csv.string() << "\",\n";
    out << "    \"final7_adaptive_forecast_csv\": \"" << adaptive_csv.string() << "\",\n";
    out << "    \"final7_level_scaled_forecast_csv\": \"" << scaled_csv.string() << "\",\n";
    out << "    \"final7_calibrated_scaled_forecast_csv\": \""
        << calibrated_csv.string() << "\"\n";
    out << "  }\n";
    out << "}\n";
    return out.str();
}

}  // namespace

int main(int argc, char** argv) {
    const std::string csv_path =
        argc >= 2 ? argv[1] : "src/tests/data/csv/link_data_2_month.csv";
    const std::filesystem::path output_dir =
        argc >= 3 ? std::filesystem::path(argv[2])
                  : std::filesystem::path(get_absolute_process_path()) /
                        "baseline_link_rolling_eval";
    const int32_t daily_harmonic_order =
        argc >= 4 ? std::atoi(argv[3]) : kDefaultDailyHarmonicOrder;
    const int32_t weekly_harmonic_order =
        argc >= 5 ? std::atoi(argv[4]) : kDefaultWeeklyHarmonicOrder;

    std::filesystem::create_directories(output_dir);
    const std::filesystem::path config_path = output_dir / "runtime_config.yaml";
    if (!WriteTextFile(config_path,
                       BuildRuntimeConfigYaml(daily_harmonic_order,
                                              weekly_harmonic_order))) {
        return 1;
    }

    std::vector<LinkPoint> points;
    if (!LoadCsv(csv_path, &points)) return 1;

    const int64_t start_bucket = points.front().bucket_id;
    const int64_t bootstrap_end_bucket = start_bucket + 2 * kWeekMinutes;
    const int64_t week3_end_bucket = start_bucket + 3 * kWeekMinutes;
    const int64_t week4_end_bucket = start_bucket + 4 * kWeekMinutes;
    const int64_t transition_end_bucket = week4_end_bucket + kDayMinutes;
    const int64_t post_train_end_bucket = transition_end_bucket + 4 * kDayMinutes;
    const int64_t final_end_bucket = post_train_end_bucket + kWeekMinutes;

    const std::size_t bootstrap_begin = 0;
    const std::size_t bootstrap_end = FindIndexGE(points, bootstrap_end_bucket);
    const std::size_t week3_begin = bootstrap_end;
    const std::size_t week3_end = FindIndexGE(points, week3_end_bucket);
    const std::size_t week4_begin = week3_end;
    const std::size_t week4_end = FindIndexGE(points, week4_end_bucket);
    const std::size_t transition_begin = week4_end;
    const std::size_t transition_end = FindIndexGE(points, transition_end_bucket);
    const std::size_t post_train_begin = transition_end;
    const std::size_t post_train_end = FindIndexGE(points, post_train_end_bucket);
    const std::size_t final_begin = post_train_end;
    const std::size_t final_end = FindIndexGE(points, final_end_bucket);

    if (bootstrap_end == 0 || week3_end <= week3_begin || week4_end <= week4_begin ||
        transition_end <= transition_begin || post_train_end <= post_train_begin ||
        final_end <= final_begin || final_end > points.size()) {
        std::cerr << "not enough data for rolling eval windows\n";
        return 1;
    }

    LoadedBaselineService env = LoadBaselineService(config_path);
    auto [task_status, task] =
        env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
    if (task_status != BaselineStatus::kOk || !task) {
        std::cerr << "create value task failed\n";
        return 1;
    }

    ValueBootstrapInput bootstrap_input;
    bootstrap_input.series_key = kSeriesKey;
    bootstrap_input.observations.reserve(bootstrap_end - bootstrap_begin);
    for (std::size_t i = bootstrap_begin; i < bootstrap_end; ++i) {
        bootstrap_input.observations.push_back(
            ValueBootstrapPoint{points[i].bucket_id, points[i].mbps, 1});
    }
    const BootstrapTrainResult train = task->Bootstrap(bootstrap_input);
    if (train.status != BaselineStatus::kOk) {
        std::cerr << "bootstrap train failed: " << train.diagnostics << "\n";
        return 1;
    }

    TrainRollingRange(task.get(), points, week3_begin, week3_end);

    EvalSummary week4_bootstrap;
    EvalSummary week4_rolling;
    EvalSummary transition;
    EvalSummary final7;
    EvalSummary final7_adaptive_forecast;
    EvalSummary final7_level_scaled_forecast;
    EvalSummary final7_calibrated_scaled_forecast;
    BandCalibration band_calibration;
    std::vector<TransitionCheckpoint> transition_checkpoints;
    const std::filesystem::path week4_csv = output_dir / "week4_comparison.csv";
    const std::filesystem::path transition_csv = output_dir / "transition_learning.csv";
    const std::filesystem::path final_csv = output_dir / "final7_rolling_predictions.csv";
    const std::filesystem::path adaptive_csv = output_dir / "final7_adaptive_forecast.csv";
    const std::filesystem::path scaled_csv = output_dir / "final7_level_scaled_forecast.csv";
    const std::filesystem::path calibrated_csv =
        output_dir / "final7_calibrated_scaled_forecast.csv";
    const std::filesystem::path summary_path = output_dir / "rolling_eval_summary.json";

    if (!WriteWeek4ComparisonAndTrainRolling(week4_csv,
                                             task.get(),
                                             points,
                                             week4_begin,
                                             week4_end,
                                             &week4_bootstrap,
                                             &week4_rolling)) {
        return 1;
    }

    if (!WriteTransitionLearning(transition_csv,
                                 task.get(),
                                 points,
                                 transition_begin,
                                 transition_end,
                                 &transition,
                                 &transition_checkpoints)) {
        return 1;
    }
    band_calibration =
        TrainRollingRangeAndCalibrateLevelScaledBand(task.get(),
                                                     points,
                                                     post_train_begin,
                                                     post_train_end);
    if (!WriteRollingPredictions(final_csv,
                                 task.get(),
                                 points,
                                 final_begin,
                                 final_end,
                                 &final7)) {
        return 1;
    }
    if (!WriteAdaptiveForecastPredictions(adaptive_csv,
                                          task.get(),
                                          points,
                                          final_begin,
                                          final_end,
                                          &final7_adaptive_forecast)) {
        return 1;
    }
    if (!WriteLevelScaledAdaptiveForecastPredictions(scaled_csv,
                                                     task.get(),
                                                     points,
                                                     final_begin,
                                                     final_end,
                                                     1.0,
                                                     &final7_level_scaled_forecast)) {
        return 1;
    }
    if (!WriteLevelScaledAdaptiveForecastPredictions(calibrated_csv,
                                                     task.get(),
                                                     points,
                                                     final_begin,
                                                     final_end,
                                                     band_calibration.multiplier,
                                                     &final7_calibrated_scaled_forecast)) {
        return 1;
    }

    assert(week4_bootstrap.count == week4_rolling.count);
    assert(week4_bootstrap.count == static_cast<uint64_t>(kWeekMinutes));
    assert(transition.count == static_cast<uint64_t>(kDayMinutes));
    assert(transition_checkpoints.size() == 5);
    assert(final7.count == static_cast<uint64_t>(kWeekMinutes));
    assert(final7_adaptive_forecast.count == static_cast<uint64_t>(kWeekMinutes));
    assert(final7_level_scaled_forecast.count == static_cast<uint64_t>(kWeekMinutes));
    assert(final7_calibrated_scaled_forecast.count == static_cast<uint64_t>(kWeekMinutes));
    assert(band_calibration.count == static_cast<uint64_t>(4 * kDayMinutes));

    if (!WriteTextFile(summary_path,
                       SummaryJson(week4_bootstrap,
                                   week4_rolling,
                                   transition,
                                   final7,
                                   final7_adaptive_forecast,
                                   final7_level_scaled_forecast,
                                   final7_calibrated_scaled_forecast,
                                   band_calibration,
                                   points,
                                   bootstrap_begin,
                                   bootstrap_end,
                                   week3_begin,
                                   week3_end,
                                   week4_begin,
                                   week4_end,
                                   transition_begin,
                                   transition_end,
                                   post_train_begin,
                                   post_train_end,
                                   final_begin,
                                   final_end,
                                   transition_checkpoints,
                                   week4_csv,
                                   transition_csv,
                                   final_csv,
                                   adaptive_csv,
                                   scaled_csv,
                                   calibrated_csv))) {
        return 1;
    }

    std::cout << "summary_json=" << summary_path << "\n";
    std::cout << "week4_bootstrap_count=" << week4_bootstrap.count
              << ", rmse_mbps=" << std::fixed << std::setprecision(6)
              << week4_bootstrap.rmse
              << ", abs_z_gt_3=" << week4_bootstrap.abs_z_gt_3_count
              << ", abs_z_gt_5=" << week4_bootstrap.abs_z_gt_5_count << "\n";
    std::cout << "week4_rolling_count=" << week4_rolling.count
              << ", rmse_mbps=" << week4_rolling.rmse
              << ", abs_z_gt_3=" << week4_rolling.abs_z_gt_3_count
              << ", abs_z_gt_5=" << week4_rolling.abs_z_gt_5_count << "\n";
    std::cout << "transition_count=" << transition.count
              << ", rmse_mbps=" << transition.rmse
              << ", abs_z_gt_3=" << transition.abs_z_gt_3_count
              << ", abs_z_gt_5=" << transition.abs_z_gt_5_count << "\n";
    std::cout << "final7_rolling_count=" << final7.count
              << ", rmse_mbps=" << final7.rmse
              << ", abs_z_gt_3=" << final7.abs_z_gt_3_count
              << ", abs_z_gt_5=" << final7.abs_z_gt_5_count << "\n";
    std::cout << "final7_adaptive_forecast_count=" << final7_adaptive_forecast.count
              << ", rmse_mbps=" << final7_adaptive_forecast.rmse
              << ", abs_z_gt_3=" << final7_adaptive_forecast.abs_z_gt_3_count
              << ", abs_z_gt_5=" << final7_adaptive_forecast.abs_z_gt_5_count << "\n";
    std::cout << "final7_level_scaled_forecast_count=" << final7_level_scaled_forecast.count
              << ", rmse_mbps=" << final7_level_scaled_forecast.rmse
              << ", abs_z_gt_3=" << final7_level_scaled_forecast.abs_z_gt_3_count
              << ", abs_z_gt_5=" << final7_level_scaled_forecast.abs_z_gt_5_count << "\n";
    std::cout << "final7_calibrated_scaled_forecast_count="
              << final7_calibrated_scaled_forecast.count
              << ", rmse_mbps=" << final7_calibrated_scaled_forecast.rmse
              << ", abs_z_gt_3=" << final7_calibrated_scaled_forecast.abs_z_gt_3_count
              << ", abs_z_gt_5=" << final7_calibrated_scaled_forecast.abs_z_gt_5_count
              << ", calibration_multiplier=" << band_calibration.multiplier << "\n";
    return 0;
}
