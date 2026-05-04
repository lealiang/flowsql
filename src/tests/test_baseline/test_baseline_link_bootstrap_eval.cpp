/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

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
#include <plugins/baseline/bootstrap/bootstrap_engine.h>
#include <plugins/baseline/config/runtime_config.h>

using namespace flowsql;
using namespace flowsql::baseline;

namespace {

constexpr int64_t kBucketSeconds = 60;
constexpr int64_t kWeekMinutes = 7 * 24 * 60;
constexpr int64_t kShanghaiUtcOffsetSeconds = 8 * 60 * 60;
constexpr double kConfidenceLevel = 0.95;
constexpr double kZ95 = 1.96;
constexpr int32_t kDefaultTrainWeeks = 2;
constexpr int32_t kDefaultPredictionWeeks = 4;

struct LinkPoint {
    std::string timestamp;
    int64_t bucket_id = 0;
    double mbps = 0.0;
};

struct EvalSummary {
    uint64_t train_count = 0;
    uint64_t prediction_count = 0;
    uint64_t inside_band_count = 0;
    double coverage_ratio = 0.0;
    double mean_abs_z = 0.0;
    double max_abs_z = 0.0;
    double rmse = 0.0;
    double mape = 0.0;
};

struct EvalAccumulator {
    uint64_t prediction_count = 0;
    uint64_t inside_band_count = 0;
    double sum_abs_z = 0.0;
    double max_abs_z = 0.0;
    double sum_square_error = 0.0;
    double sum_abs_percentage_error = 0.0;

    void Add(double actual, const BootstrapPrediction& prediction, double z_score, bool in_band) {
        if (in_band) ++inside_band_count;
        const double error = actual - prediction.baseline_mu;
        const double abs_z = std::fabs(z_score);
        sum_abs_z += abs_z;
        max_abs_z = std::max(max_abs_z, abs_z);
        sum_square_error += error * error;
        if (std::fabs(actual) > 1.0e-9) {
            sum_abs_percentage_error += std::fabs(error / actual);
        }
        ++prediction_count;
    }

    EvalSummary Finish() const {
        EvalSummary summary;
        summary.prediction_count = prediction_count;
        summary.inside_band_count = inside_band_count;
        summary.max_abs_z = max_abs_z;
        if (prediction_count > 0) {
            const double count = static_cast<double>(prediction_count);
            summary.coverage_ratio = static_cast<double>(inside_band_count) / count;
            summary.mean_abs_z = sum_abs_z / count;
            summary.rmse = std::sqrt(sum_square_error / count);
            summary.mape = sum_abs_percentage_error / count;
        }
        return summary;
    }
};

struct WeeklyEvalSummary {
    int32_t week_number = 0;
    std::string start_time;
    std::string end_time;
    EvalSummary metrics;
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
    *out_bucket_id = (static_cast<int64_t>(utc_if_local_was_utc) -
                      kShanghaiUtcOffsetSeconds) /
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

BaselineTaskSpec BuildValueSpec() {
    BaselineTaskSpec spec;
    spec.task_id = "link_bps_bootstrap_eval";
    spec.name = "link bps bootstrap eval";
    spec.task_kind = "value";
    spec.feature_id = "link_bps_mbps";
    spec.feature = spec.feature_id;
    spec.feature_type = "value_basic";
    spec.profile = "default";
    spec.clock_spec.bucket_seconds = kBucketSeconds;
    spec.clock_spec.timezone = "Asia/Shanghai";
    spec.calendar_ref.calendar_id = "cn-holiday";
    spec.calendar_ref.calendar_version = "2026.1";
    spec.delta = kBucketSeconds;
    spec.tz = "Asia/Shanghai";
    return spec;
}

double DirectionalZScore(const BootstrapPrediction& prediction, double actual) {
    const double upper_sigma = (prediction.baseline_upper - prediction.baseline_mu) / kZ95;
    const double lower_sigma = (prediction.baseline_mu - prediction.baseline_lower) / kZ95;
    const double fallback_sigma = std::max(prediction.band_width / (2.0 * kZ95), 1.0e-9);
    double sigma = fallback_sigma;
    if (actual >= prediction.baseline_mu && upper_sigma > 1.0e-9) {
        sigma = upper_sigma;
    } else if (actual < prediction.baseline_mu && lower_sigma > 1.0e-9) {
        sigma = lower_sigma;
    }
    return (actual - prediction.baseline_mu) / sigma;
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

bool LoadRuntimeConfigForEval(int32_t daily_harmonic_order,
                              int32_t weekly_harmonic_order,
                              const std::filesystem::path& output_dir) {
    if (daily_harmonic_order <= 0 || weekly_harmonic_order < 0) {
        std::cerr << "invalid harmonic order\n";
        return false;
    }
    std::filesystem::create_directories(output_dir);
    const std::filesystem::path config_path = output_dir / "runtime_config.yaml";
    if (!WriteTextFile(config_path,
                       BuildRuntimeConfigYaml(daily_harmonic_order,
                                              weekly_harmonic_order))) {
        return false;
    }
    std::string err;
    const int rc = LoadBaselineRuntimeConfigFromYaml(config_path.string(), true, &err);
    if (rc != 0) {
        std::cerr << "load runtime config failed: " << err << "\n";
        return false;
    }
    return true;
}

bool WritePredictions(const std::filesystem::path& path,
                      const std::vector<LinkPoint>& points,
                      std::size_t predict_start_index,
                      std::size_t predict_end_index,
                      int32_t train_weeks,
                      int32_t prediction_weeks,
                      const BootstrapArtifact& artifact,
                      EvalSummary* out_summary,
                      std::vector<WeeklyEvalSummary>* out_weekly) {
    if (!out_summary) return false;
    *out_summary = EvalSummary{};
    if (out_weekly) out_weekly->clear();
    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "failed to open prediction output: " << path << "\n";
        return false;
    }

    BootstrapEngine engine;
    BootstrapPredictionOptions options;
    options.confidence_level = kConfidenceLevel;

    file << "timestamp,bucket_id,actual_mbps,baseline_mu_mbps,baseline_lower_mbps,"
            "baseline_upper_mbps,band_width_mbps,z_score,in_band\n";

    EvalAccumulator total_accumulator;
    std::vector<EvalAccumulator> weekly_accumulators(
        static_cast<std::size_t>(prediction_weeks));
    std::vector<std::size_t> weekly_start_index(static_cast<std::size_t>(prediction_weeks),
                                                points.size());
    std::vector<std::size_t> weekly_end_index(static_cast<std::size_t>(prediction_weeks),
                                              points.size());

    const int64_t start_bucket_id = points[predict_start_index].bucket_id;
    const int64_t end_bucket_id = points[predict_end_index - 1].bucket_id;
    if (end_bucket_id < start_bucket_id) {
        std::cerr << "invalid prediction bucket range\n";
        return false;
    }
    const uint64_t sequence_bucket_count =
        static_cast<uint64_t>(end_bucket_id - start_bucket_id + 1);
    if (sequence_bucket_count > std::numeric_limits<uint32_t>::max()) {
        std::cerr << "prediction bucket range is too large for sequence API\n";
        return false;
    }
    const BootstrapPredictionSequence prediction_sequence =
        engine.PredictValueSequence(artifact,
                                    start_bucket_id,
                                    static_cast<uint32_t>(sequence_bucket_count),
                                    options);
    if (prediction_sequence.status != BaselineStatus::kOk ||
        prediction_sequence.predictions.size() != sequence_bucket_count) {
        std::cerr << "batch prediction failed, status="
                  << static_cast<int>(prediction_sequence.status) << "\n";
        return false;
    }

    for (std::size_t i = predict_start_index; i < predict_end_index; ++i) {
        const LinkPoint& point = points[i];
        if (point.bucket_id < start_bucket_id) {
            std::cerr << "prediction point is before batch start bucket: " << point.bucket_id << "\n";
            return false;
        }
        const uint64_t prediction_offset =
            static_cast<uint64_t>(point.bucket_id - start_bucket_id);
        if (prediction_offset >= prediction_sequence.predictions.size()) {
            std::cerr << "prediction point is outside batch range: " << point.bucket_id << "\n";
            return false;
        }
        const BootstrapPrediction& prediction =
            prediction_sequence.predictions[static_cast<std::size_t>(prediction_offset)];
        if (prediction.status != BaselineStatus::kOk) {
            std::cerr << "prediction failed at bucket: " << point.bucket_id << "\n";
            return false;
        }
        if (prediction.bucket_id != point.bucket_id) {
            std::cerr << "batch prediction bucket mismatch: expected " << point.bucket_id
                      << ", got " << prediction.bucket_id << "\n";
            return false;
        }
        const double z_score = DirectionalZScore(prediction, point.mbps);
        const bool in_band =
            point.mbps >= prediction.baseline_lower && point.mbps <= prediction.baseline_upper;

        total_accumulator.Add(point.mbps, prediction, z_score, in_band);
        const int64_t week_offset =
            (point.bucket_id - points[predict_start_index].bucket_id) /
            kWeekMinutes;
        if (week_offset >= 0 && week_offset < prediction_weeks) {
            const std::size_t week_index = static_cast<std::size_t>(week_offset);
            weekly_accumulators[week_index].Add(point.mbps, prediction, z_score, in_band);
            if (weekly_start_index[week_index] == points.size()) {
                weekly_start_index[week_index] = i;
            }
            weekly_end_index[week_index] = i;
        }

        file << point.timestamp << ',' << point.bucket_id << ',' << std::fixed
             << std::setprecision(6) << point.mbps << ',' << prediction.baseline_mu << ','
             << prediction.baseline_lower << ',' << prediction.baseline_upper << ','
             << prediction.band_width << ',' << z_score << ',' << (in_band ? 1 : 0) << '\n';
    }

    *out_summary = total_accumulator.Finish();
    if (out_weekly) {
        out_weekly->reserve(static_cast<std::size_t>(prediction_weeks));
        for (std::size_t i = 0; i < weekly_accumulators.size(); ++i) {
            WeeklyEvalSummary item;
            item.week_number = static_cast<int32_t>(train_weeks + i + 1);
            item.metrics = weekly_accumulators[i].Finish();
            if (weekly_start_index[i] != points.size()) {
                item.start_time = points[weekly_start_index[i]].timestamp;
                item.end_time = points[weekly_end_index[i]].timestamp;
            }
            out_weekly->push_back(std::move(item));
        }
    }
    return file.good();
}

void AppendMetricsJson(std::ostringstream* out, const EvalSummary& summary, const std::string& indent) {
    if (!out) return;
    *out << indent << "\"inside_band_count\": " << summary.inside_band_count << ",\n";
    *out << indent << "\"coverage_ratio\": " << std::fixed << std::setprecision(6)
         << summary.coverage_ratio << ",\n";
    *out << indent << "\"mean_abs_z\": " << summary.mean_abs_z << ",\n";
    *out << indent << "\"max_abs_z\": " << summary.max_abs_z << ",\n";
    *out << indent << "\"rmse_mbps\": " << summary.rmse << ",\n";
    *out << indent << "\"mape\": " << summary.mape << "\n";
}

std::string SummaryJson(const EvalSummary& summary,
                        const std::vector<WeeklyEvalSummary>& weekly,
                        const std::vector<LinkPoint>& points,
                        std::size_t predict_start_index,
                        std::size_t predict_end_index,
                        const BootstrapTrainResult& train_result,
                        int32_t daily_harmonic_order,
                        int32_t weekly_harmonic_order,
                        int32_t train_weeks,
                        int32_t prediction_weeks,
                        const std::filesystem::path& artifact_path,
                        const std::filesystem::path& seed_path,
                        const std::filesystem::path& prediction_path) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema_version\": 1,\n";
    out << "  \"document_kind\": \"baseline_bootstrap_eval_summary\",\n";
    out << "  \"csv\": \"src/tests/data/csv/link_data_2_month.csv\",\n";
    out << "  \"feature\": \"link_bps_mbps\",\n";
    out << "  \"unit\": \"Mbps\",\n";
    out << "  \"bucket_seconds\": " << kBucketSeconds << ",\n";
    out << "  \"timezone\": \"Asia/Shanghai\",\n";
    out << "  \"confidence_level\": " << kConfidenceLevel << ",\n";
    out << "  \"harmonic_order\": {\n";
    out << "    \"daily\": " << daily_harmonic_order << ",\n";
    out << "    \"weekly\": " << weekly_harmonic_order << "\n";
  out << "  },\n";
    out << "  \"history_window\": {\n";
    out << "    \"weeks\": " << train_weeks << ",\n";
    out << "    \"days\": " << (train_weeks * 7) << ",\n";
    out << "    \"start_time\": \"" << points.front().timestamp << "\",\n";
    out << "    \"end_time\": \"" << points[predict_start_index - 1].timestamp << "\",\n";
    out << "    \"accepted_count\": " << train_result.accepted_count << "\n";
    out << "  },\n";
    out << "  \"prediction_window\": {\n";
    out << "    \"weeks\": " << prediction_weeks << ",\n";
    out << "    \"start_time\": \"" << points[predict_start_index].timestamp << "\",\n";
    out << "    \"end_time\": \"" << points[predict_end_index - 1].timestamp << "\",\n";
    out << "    \"count\": " << summary.prediction_count << "\n";
    out << "  },\n";
    out << "  \"metrics\": {\n";
    AppendMetricsJson(&out, summary, "    ");
    out << "  },\n";
    out << "  \"weekly_metrics\": [\n";
    for (std::size_t i = 0; i < weekly.size(); ++i) {
        const auto& item = weekly[i];
        out << "    {\n";
        out << "      \"week_number\": " << item.week_number << ",\n";
        out << "      \"start_time\": \"" << item.start_time << "\",\n";
        out << "      \"end_time\": \"" << item.end_time << "\",\n";
        out << "      \"prediction_count\": " << item.metrics.prediction_count << ",\n";
        out << "      \"metrics\": {\n";
        AppendMetricsJson(&out, item.metrics, "        ");
        out << "      }\n";
        out << "    }" << (i + 1 < weekly.size() ? "," : "") << "\n";
    }
    out << "  ],\n";
    out << "  \"outputs\": {\n";
    out << "    \"artifact_json\": \"" << artifact_path.string() << "\",\n";
    out << "    \"seed_json\": \"" << seed_path.string() << "\",\n";
    out << "    \"prediction_csv\": \"" << prediction_path.string() << "\"\n";
    out << "  }\n";
    out << "}\n";
    return out.str();
}

}  // namespace

int main(int argc, char** argv) {
    const std::string csv_path =
        argc >= 2 ? argv[1] : "src/tests/data/csv/link_data_2_month.csv";
    const int32_t daily_harmonic_order = argc >= 4 ? std::atoi(argv[3]) : 6;
    const int32_t weekly_harmonic_order = argc >= 5 ? std::atoi(argv[4]) : 3;
    const int32_t train_weeks = argc >= 6 ? std::atoi(argv[5]) : kDefaultTrainWeeks;
    const int32_t prediction_weeks =
        argc >= 7 ? std::atoi(argv[6]) : kDefaultPredictionWeeks;

    const std::filesystem::path output_dir =
        argc >= 3
            ? std::filesystem::path(argv[2])
            : std::filesystem::path(get_absolute_process_path()) /
                  ("baseline_link_bootstrap_eval_train" + std::to_string(train_weeks) +
                   "_predict" + std::to_string(prediction_weeks) + "_daily" +
                   std::to_string(daily_harmonic_order) + "_week" +
                   std::to_string(weekly_harmonic_order));

    if (train_weeks <= 0 || prediction_weeks <= 0) {
        std::cerr << "train_weeks and prediction_weeks must be positive\n";
        return 1;
    }

    if (!LoadRuntimeConfigForEval(daily_harmonic_order, weekly_harmonic_order, output_dir)) {
        return 1;
    }

    std::vector<LinkPoint> points;
    if (!LoadCsv(csv_path, &points)) return 1;

    const int64_t train_start_bucket = points.front().bucket_id;
    const int64_t train_end_exclusive =
        train_start_bucket + static_cast<int64_t>(train_weeks) * kWeekMinutes;
    const int64_t predict_end_exclusive =
        train_end_exclusive + static_cast<int64_t>(prediction_weeks) * kWeekMinutes;
    ValueBootstrapInput input;
    input.series_key = "link-traffic-bps";

    std::size_t predict_start_index = points.size();
    std::size_t predict_end_index = points.size();
    for (std::size_t i = 0; i < points.size(); ++i) {
        const LinkPoint& point = points[i];
        if (point.bucket_id < train_end_exclusive) {
            input.observations.push_back(ValueBootstrapPoint{point.bucket_id, point.mbps, 1});
        } else {
            predict_start_index = i;
            break;
        }
    }
    for (std::size_t i = predict_start_index; i < points.size(); ++i) {
        if (points[i].bucket_id >= predict_end_exclusive) {
            predict_end_index = i;
            break;
        }
    }
    if (input.observations.empty() || predict_start_index >= points.size() ||
        predict_end_index <= predict_start_index) {
        std::cerr << "not enough train or prediction data\n";
        return 1;
    }

    BootstrapEngine engine;
    BootstrapArtifact artifact;
    const BootstrapTrainResult train_result = engine.TrainValue(BuildValueSpec(), input, &artifact);
    if (train_result.status != BaselineStatus::kOk) {
        std::cerr << "bootstrap train failed, status=" << static_cast<int>(train_result.status)
                  << ", diagnostics=" << train_result.diagnostics << "\n";
        return 1;
    }

    auto [artifact_status, artifact_json] =
        engine.ExportArtifact(artifact, BaselineSerializationFormat::kJson);
    if (artifact_status != BaselineStatus::kOk) {
        std::cerr << "artifact export failed\n";
        return 1;
    }

    BootstrapSeed seed;
    const BaselineStatus seed_status = engine.ExportSeed(artifact, &seed);
    if (seed_status != BaselineStatus::kOk) {
        std::cerr << "seed build failed\n";
        return 1;
    }
    auto [seed_export_status, seed_json] =
        engine.ExportSeed(seed, BaselineSerializationFormat::kJson);
    if (seed_export_status != BaselineStatus::kOk) {
        std::cerr << "seed export failed\n";
        return 1;
    }

    std::filesystem::create_directories(output_dir);
    const std::filesystem::path artifact_path = output_dir / "bootstrap_artifact.json";
    const std::filesystem::path seed_path = output_dir / "bootstrap_seed.json";
    const std::filesystem::path prediction_path = output_dir / "prediction_results.csv";
    const std::filesystem::path summary_path = output_dir / "prediction_summary.json";

    if (!WriteTextFile(artifact_path, artifact_json)) return 1;
    if (!WriteTextFile(seed_path, seed_json)) return 1;

    EvalSummary summary;
    summary.train_count = input.observations.size();
    std::vector<WeeklyEvalSummary> weekly;
    if (!WritePredictions(prediction_path,
                          points,
                          predict_start_index,
                          predict_end_index,
                          train_weeks,
                          prediction_weeks,
                          artifact,
                          &summary,
                          &weekly)) {
        return 1;
    }
    if (!WriteTextFile(summary_path,
                       SummaryJson(summary,
                                   weekly,
                                   points,
                                   predict_start_index,
                                   predict_end_index,
                                   train_result,
                                   daily_harmonic_order,
                                   weekly_harmonic_order,
                                   train_weeks,
                                   prediction_weeks,
                                   artifact_path,
                                   seed_path,
                                   prediction_path))) {
        return 1;
    }

    std::cout << "artifact_json=" << artifact_path << "\n";
    std::cout << "seed_json=" << seed_path << "\n";
    std::cout << "prediction_csv=" << prediction_path << "\n";
    std::cout << "summary_json=" << summary_path << "\n";
    std::cout << "train_count=" << input.observations.size()
              << ", prediction_count=" << summary.prediction_count
              << ", coverage_ratio=" << std::fixed << std::setprecision(6)
              << summary.coverage_ratio << ", mean_abs_z=" << summary.mean_abs_z
              << ", rmse_mbps=" << summary.rmse << "\n";
    for (const auto& item : weekly) {
        std::cout << "week" << item.week_number
                  << "_count=" << item.metrics.prediction_count
                  << ", coverage_ratio=" << item.metrics.coverage_ratio
                  << ", mean_abs_z=" << item.metrics.mean_abs_z
                  << ", rmse_mbps=" << item.metrics.rmse << "\n";
    }
    ResetBaselineRuntimeConfig();
    return 0;
}
