/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <common/error_code.h>
#include <framework/interfaces/ibaseline_service.h>

#include "test_common.h"

namespace flowsql {
namespace baseline_test {
namespace {

constexpr int64_t kSecondsPerDay = 24 * 60 * 60;
constexpr int kDefaultBootstrapDays = 14;
constexpr int kDefaultTopK = 20;
constexpr int kSnapshotPollInterval = 60;

struct CliOptions {
    std::string mode = "replay";
    std::string csv_path;
    std::string report_dir = "build/output";
    std::string task_name = "link_bps_mbps";
    std::string key_field = "link";
    std::string series_key = "link_data_2_month";
    std::string feature = "bps_mbps";
    std::string feature_profile = "traffic";
    std::string feature_type = "value_basic";
    std::string tz = "Asia/Shanghai";
    int64_t delta = 60;
    int bootstrap_days = kDefaultBootstrapDays;
    int topk = kDefaultTopK;
};

struct SeriesPoint {
    std::string time_text;
    int64_t bucket_id = 0;
    double value = 0.0;
    uint64_t sample_count = 0;
};

struct SnapshotState {
    bool formal_ready = false;
    bool shadow_active = false;
    uint64_t formal_model_version = 0;
    std::string model_state;
    std::string switch_state;
    std::string candidate_state;
    std::string drift_direction;
    std::string failure_reason;
    std::string failure_reason_detail;
};

struct ScoredPoint {
    std::string time_text;
    int64_t bucket_id = 0;
    double value = 0.0;
    double normalized_score = 0.0;
    double confidence = 0.0;
    uint32_t persistence = 0;
    BaselineProvider provider = BaselineProvider::kNone;
    BaselineReasonCode reason = BaselineReasonCode::kUnknown;
    BaselineDirection direction = BaselineDirection::kUnknown;
    uint64_t flags = 0;
};

struct TraceRow {
    std::string phase;
    std::string time_text;
    int64_t bucket_id = 0;
    double actual_value = 0.0;
    bool has_baseline = false;
    double baseline_value = 0.0;
    double baseline_internal = 0.0;
    double residual = 0.0;
    double z_score = 0.0;
    double p_shift = 0.0;
    double score_point = 0.0;
    double score_shift = 0.0;
    double normalized_score = 0.0;
    double confidence = 0.0;
    uint32_t persistence = 0;
    BaselineProvider provider = BaselineProvider::kNone;
    BaselineReasonCode reason = BaselineReasonCode::kUnknown;
    BaselineDirection direction = BaselineDirection::kUnknown;
    uint64_t flags = 0;
    bool shadow_active = false;
};

struct TriggerPoint {
    std::string trigger_type;
    std::string time_text;
    int64_t bucket_id = 0;
    double actual_value = 0.0;
    bool has_baseline = false;
    double baseline_value = 0.0;
    double score_point = 0.0;
    double score_shift = 0.0;
    double normalized_score = 0.0;
    std::string provider;
    std::string reason;
    std::string flags;
    std::string detail;
};

struct EventWindow {
    std::string event_type;
    std::string start_time;
    std::string end_time;
    int64_t start_bucket = 0;
    int64_t end_bucket = 0;
    std::size_t point_count = 0;
    std::string start_reason;
    std::string start_flags;
    bool shadow_involved = false;
    bool rebuild_queued = false;
    std::string peak_time;
    int64_t peak_bucket = 0;
    double peak_value = 0.0;
    bool peak_has_baseline = false;
    double peak_baseline = 0.0;
    double peak_score_point = 0.0;
    double peak_score_shift = 0.0;
    double peak_normalized_score = 0.0;
};

struct CapabilityItem {
    std::string name;
    bool observed = false;
    std::string detail;
};

struct ReplaySummary {
    std::size_t total_points = 0;
    std::size_t positive_score_count = 0;
    std::size_t medium_or_higher_count = 0;
    std::size_t high_severity_count = 0;
    std::size_t cold_start_count = 0;
    std::size_t rebuild_queued_count = 0;
    std::size_t shadow_result_count = 0;
    std::size_t history_fetch_count = 0;

    bool bootstrap_rebuild_requested = false;
    bool bootstrap_formal_ready = false;
    bool saw_rebuild_queued = false;
    bool saw_shadow_active = false;
    bool saw_formal_switch = false;

    std::string start_time;
    std::string end_time;
    std::string first_rebuild_queued_time;
    std::string first_shadow_time;
    std::string first_switch_time;

    int64_t bootstrap_bucket_id = 0;
    int64_t first_rebuild_queued_bucket = 0;
    int64_t first_shadow_bucket = 0;
    int64_t first_switch_bucket = 0;

    uint64_t bootstrap_formal_model_version = 0;
    SnapshotState bootstrap_snapshot;
    SnapshotState final_snapshot;
    std::string bootstrap_snapshot_json;
    std::string final_snapshot_json;

    std::vector<double> pre_shift_formal_scores;
    std::vector<ScoredPoint> scored_points;
    std::vector<TraceRow> trace_rows;
    std::vector<TriggerPoint> trigger_points;
    std::vector<EventWindow> event_windows;
    bool manual_switch_validation_requested = false;
};

BaselineStringRef StringRef(const std::string& text) {
    return BaselineStringRef{text.c_str(), static_cast<uint32_t>(text.size())};
}

const char* ProviderName(BaselineProvider provider) {
    switch (provider) {
        case BaselineProvider::kFormal:
            return "formal";
        case BaselineProvider::kShadow:
            return "shadow";
        case BaselineProvider::kSource:
            return "source";
        case BaselineProvider::kNone:
            return "none";
    }
    return "unknown";
}

const char* ReasonName(BaselineReasonCode reason) {
    switch (reason) {
        case BaselineReasonCode::kSpike:
            return "spike";
        case BaselineReasonCode::kDrop:
            return "drop";
        case BaselineReasonCode::kBaselineShiftUp:
            return "baseline_shift_up";
        case BaselineReasonCode::kBaselineShiftDown:
            return "baseline_shift_down";
        case BaselineReasonCode::kDrift:
            return "drift";
        case BaselineReasonCode::kScan:
            return "scan";
        case BaselineReasonCode::kRarePeer:
            return "rare_peer";
        case BaselineReasonCode::kUnknown:
            return "unknown";
    }
    return "unknown";
}

const char* DirectionName(BaselineDirection direction) {
    switch (direction) {
        case BaselineDirection::kUp:
            return "up";
        case BaselineDirection::kDown:
            return "down";
        case BaselineDirection::kUnknown:
            return "unknown";
    }
    return "unknown";
}

double InverseTransformValue(const std::string& transform_name, double internal_value) {
    if (transform_name == "identity") return internal_value;
    return std::expm1(internal_value);
}

std::string FlagsToString(uint64_t flags) {
    if (flags == 0) return "none";
    std::string text;
    auto append_flag = [&text](const char* name) {
        if (!text.empty()) text.append("|");
        text.append(name);
    };
    if ((flags & kBaselineFlagColdStart) != 0) append_flag("cold_start");
    if ((flags & kBaselineFlagGapBefore) != 0) append_flag("gap_before");
    if ((flags & kBaselineFlagOutOfOrder) != 0) append_flag("out_of_order");
    if ((flags & kBaselineFlagRebuildQueued) != 0) append_flag("rebuild_queued");
    if ((flags & kBaselineFlagShadowActive) != 0) append_flag("shadow_active");
    return text;
}

std::string CsvEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char ch : value) {
        if (ch == '"') escaped.push_back('"');
        escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
}

bool IsTraceEventActive(const TraceRow& row) {
    return row.normalized_score > 0.0 || row.score_shift > 0.0 ||
           (row.flags & kBaselineFlagRebuildQueued) != 0;
}

std::string TrimCopy(const std::string& input) {
    std::size_t begin = 0;
    std::size_t end = input.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
        ++begin;
    }
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }
    return input.substr(begin, end - begin);
}

bool ParsePositiveInt(const std::string& text, int* out) {
    if (!out) return false;
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' || value <= 0 ||
        value > std::numeric_limits<int>::max()) {
        return false;
    }
    *out = static_cast<int>(value);
    return true;
}

bool ParsePositiveInt64(const std::string& text, int64_t* out) {
    if (!out) return false;
    char* end = nullptr;
    errno = 0;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' || value <= 0) return false;
    *out = static_cast<int64_t>(value);
    return true;
}

void PrintUsage() {
    std::printf(
        "Usage:\n"
        "  test_baseline_value_replay_example --mode=replay --csv=/path/to/file.csv [options]\n"
        "  test_baseline_value_replay_example --mode=demo [options]\n"
        "\n"
        "Options:\n"
        "  --mode=replay|demo            运行真实 CSV 回放或内置接入样例\n"
        "  --csv=PATH                    CSV 路径，仅 replay 模式必填\n"
        "  --report-dir=DIR              报告输出目录，默认 build/output\n"
        "  --task-name=NAME              baseline 任务名\n"
        "  --key-field=NAME              任务配置里的 key 字段名，默认 link\n"
        "  --series-key=VALUE            实际观测 key 值，默认 link_data_2_month\n"
        "  --feature=NAME                任务 feature，默认 bps_mbps\n"
        "  --feature-profile=NAME        value_basic profile，默认 traffic\n"
        "  --tz=TZ                       时区，默认 Asia/Shanghai\n"
        "  --delta=SECONDS               桶宽，默认 60\n"
        "  --bootstrap-days=N            首次手工重建前预热天数，默认 14\n"
        "  --topk=N                      输出 topK 异常点数量，默认 20\n");
}

bool ParseArgs(int argc, char** argv, CliOptions* options, std::string* err) {
    if (!options) return false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            PrintUsage();
            std::exit(0);
        }
        const std::size_t pos = arg.find('=');
        if (pos == std::string::npos) {
            if (err) *err = "invalid argument: " + arg;
            return false;
        }
        const std::string name = arg.substr(0, pos);
        const std::string value = arg.substr(pos + 1);
        if (name == "--mode") {
            options->mode = value;
        } else if (name == "--csv") {
            options->csv_path = value;
        } else if (name == "--report-dir") {
            options->report_dir = value;
        } else if (name == "--task-name") {
            options->task_name = value;
        } else if (name == "--key-field") {
            options->key_field = value;
        } else if (name == "--series-key") {
            options->series_key = value;
        } else if (name == "--feature") {
            options->feature = value;
        } else if (name == "--feature-profile") {
            options->feature_profile = value;
        } else if (name == "--tz") {
            options->tz = value;
        } else if (name == "--delta") {
            if (!ParsePositiveInt64(value, &options->delta)) {
                if (err) *err = "delta must be a positive integer";
                return false;
            }
        } else if (name == "--bootstrap-days") {
            if (!ParsePositiveInt(value, &options->bootstrap_days)) {
                if (err) *err = "bootstrap-days must be a positive integer";
                return false;
            }
        } else if (name == "--topk") {
            if (!ParsePositiveInt(value, &options->topk)) {
                if (err) *err = "topk must be a positive integer";
                return false;
            }
        } else {
            if (err) *err = "unknown argument: " + name;
            return false;
        }
    }

    if (options->mode != "replay" && options->mode != "demo") {
        if (err) *err = "mode must be replay or demo";
        return false;
    }
    if (options->mode == "replay" && options->csv_path.empty()) {
        if (err) *err = "replay mode requires --csv";
        return false;
    }
    if (options->task_name.empty() || options->key_field.empty() ||
        options->series_key.empty() || options->feature.empty() ||
        options->feature_profile.empty() || options->tz.empty()) {
        if (err) *err = "task-name/key-field/series-key/feature/feature-profile/tz must not be empty";
        return false;
    }
    return true;
}

class ScopedTimezone {
 public:
    explicit ScopedTimezone(const std::string& tz) {
        const char* current = std::getenv("TZ");
        if (current != nullptr) {
            had_previous_ = true;
            previous_ = current;
        }
        if (setenv("TZ", tz.c_str(), 1) != 0) return;
        tzset();
        active_ = true;
    }

    ~ScopedTimezone() {
        if (!active_) return;
        if (had_previous_) {
            setenv("TZ", previous_.c_str(), 1);
        } else {
            unsetenv("TZ");
        }
        tzset();
    }

    bool active() const { return active_; }

 private:
    bool active_ = false;
    bool had_previous_ = false;
    std::string previous_;
};

bool ParseTimestamp(const std::string& time_text, std::tm* out) {
    if (!out) return false;
    const std::string text = TrimCopy(time_text);
    const char* formats[] = {
        "%Y/%m/%d %H:%M:%S",
        "%Y/%m/%d %H:%M",
        "%Y-%m-%d %H:%M:%S",
        "%Y-%m-%d %H:%M",
    };
    for (const char* format : formats) {
        std::tm parsed{};
        char* end = strptime(text.c_str(), format, &parsed);
        if (end != nullptr && *end == '\0') {
            parsed.tm_isdst = -1;
            *out = parsed;
            return true;
        }
    }
    return false;
}

bool LoadCsvSeries(const CliOptions& options,
                   std::vector<SeriesPoint>* out_points,
                   std::string* err) {
    if (!out_points) return false;
    std::ifstream input(options.csv_path);
    if (!input.is_open()) {
        if (err) *err = "failed to open csv: " + options.csv_path;
        return false;
    }

    ScopedTimezone scoped_tz(options.tz);
    if (!scoped_tz.active()) {
        if (err) *err = "failed to activate timezone: " + options.tz;
        return false;
    }

    std::vector<SeriesPoint> points;
    std::string line;
    int line_no = 0;
    while (std::getline(input, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string trimmed = TrimCopy(line);
        if (trimmed.empty()) continue;
        if (line_no == 1 && trimmed.rfind("time,", 0) == 0) continue;

        const std::size_t comma = trimmed.find_last_of(',');
        if (comma == std::string::npos) {
            if (err) *err = "csv parse failed at line " + std::to_string(line_no);
            return false;
        }

        const std::string time_text = TrimCopy(trimmed.substr(0, comma));
        const std::string value_text = TrimCopy(trimmed.substr(comma + 1));

        std::tm tm{};
        if (!ParseTimestamp(time_text, &tm)) {
            if (err) {
                *err = "timestamp parse failed at line " + std::to_string(line_no) +
                       ": " + time_text;
            }
            return false;
        }
        errno = 0;
        char* end = nullptr;
        const double value = std::strtod(value_text.c_str(), &end);
        if (errno != 0 || end == value_text.c_str() || *end != '\0') {
            if (err) {
                *err = "value parse failed at line " + std::to_string(line_no) +
                       ": " + value_text;
            }
            return false;
        }

        const std::time_t epoch = std::mktime(&tm);
        if (epoch == static_cast<std::time_t>(-1)) {
            if (err) {
                *err = "mktime failed at line " + std::to_string(line_no) +
                       ": " + time_text;
            }
            return false;
        }
        const int64_t bucket_id = static_cast<int64_t>(epoch) / options.delta;
        if (!points.empty() && bucket_id <= points.back().bucket_id) {
            if (err) {
                *err = "bucket_id must be strictly increasing, failed at line " +
                       std::to_string(line_no);
            }
            return false;
        }

        SeriesPoint point;
        point.time_text = time_text;
        point.bucket_id = bucket_id;
        point.value = value;
        points.push_back(std::move(point));
    }

    if (points.empty()) {
        if (err) *err = "csv contains no data rows";
        return false;
    }
    *out_points = std::move(points);
    return true;
}

class VectorValueHistoryReader : public IBaselineValueHistoryReader {
 public:
    VectorValueHistoryReader(std::string series_key, const std::vector<SeriesPoint>* points)
        : series_key_(std::move(series_key)), points_(points) {}

    int Fetch(const HistoryFetchRequest& req,
              std::function<int(const ValueObservation&)> on_point) override {
        if (!points_) return error::BAD_REQUEST;
        ++fetch_count_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_fetch_start_ = req.bucket_start;
            last_fetch_end_ = req.bucket_end;
        }
        if (!on_point) return error::OK;

        auto begin = std::lower_bound(points_->begin(),
                                      points_->end(),
                                      req.bucket_start,
                                      [](const SeriesPoint& point, int64_t bucket_id) {
                                          return point.bucket_id < bucket_id;
                                      });
        auto end = std::upper_bound(points_->begin(),
                                    points_->end(),
                                    req.bucket_end,
                                    [](int64_t bucket_id, const SeriesPoint& point) {
                                        return bucket_id < point.bucket_id;
                                    });
        const BaselineStringRef key = StringRef(series_key_);
        for (auto it = begin; it != end; ++it) {
            const ValueObservation obs{key, it->bucket_id, it->value, it->sample_count};
            const int rc = on_point(obs);
            if (rc != error::OK) return rc;
        }
        return error::OK;
    }

    std::size_t fetch_count() const { return fetch_count_.load(); }

    int64_t last_fetch_start() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_fetch_start_;
    }

    int64_t last_fetch_end() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_fetch_end_;
    }

 private:
    std::string series_key_;
    const std::vector<SeriesPoint>* points_ = nullptr;
    std::atomic<std::size_t> fetch_count_{0};
    mutable std::mutex mutex_;
    int64_t last_fetch_start_ = 0;
    int64_t last_fetch_end_ = 0;
};

std::string BuildValueTaskConfigJson(const CliOptions& options) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("name");
    writer.String(options.task_name.c_str());
    writer.Key("key");
    writer.String(options.key_field.c_str());
    writer.Key("feature");
    writer.String(options.feature.c_str());
    writer.Key("feature_type");
    writer.String(options.feature_type.c_str());
    writer.Key("feature_profile");
    writer.String(options.feature_profile.c_str());
    writer.Key("delta");
    writer.Int64(options.delta);
    writer.Key("tz");
    writer.String(options.tz.c_str());
    writer.EndObject();
    return buffer.GetString();
}

bool ReadSnapshot(IBaselineValueTask* task,
                  const std::string& series_key,
                  SnapshotState* out,
                  std::string* out_json,
                  std::string* err) {
    if (!task || !out) return false;
    std::string json;
    const int rc = task->QuerySeriesSnapshotJson(StringRef(series_key), &json);
    if (rc != error::OK) {
        if (err) *err = "QuerySeriesSnapshotJson failed: " + std::to_string(rc);
        return false;
    }

    rapidjson::Document doc;
    doc.Parse(json.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        if (err) *err = "snapshot json parse failed";
        return false;
    }

    SnapshotState snapshot;
    snapshot.formal_ready = doc.HasMember("formal_ready") && doc["formal_ready"].IsBool()
                                ? doc["formal_ready"].GetBool()
                                : false;
    snapshot.shadow_active = doc.HasMember("shadow_active") && doc["shadow_active"].IsBool()
                                 ? doc["shadow_active"].GetBool()
                                 : false;
    snapshot.formal_model_version =
        doc.HasMember("formal_model_version") && doc["formal_model_version"].IsUint64()
            ? doc["formal_model_version"].GetUint64()
            : 0;
    snapshot.model_state = doc.HasMember("model_state") && doc["model_state"].IsString()
                               ? doc["model_state"].GetString()
                               : "";
    snapshot.switch_state = doc.HasMember("switch_state") && doc["switch_state"].IsString()
                                ? doc["switch_state"].GetString()
                                : "";
    snapshot.candidate_state =
        doc.HasMember("candidate_state") && doc["candidate_state"].IsString()
            ? doc["candidate_state"].GetString()
            : "";
    snapshot.drift_direction = doc.HasMember("drift_direction") && doc["drift_direction"].IsString()
                                   ? doc["drift_direction"].GetString()
                                   : "";
    snapshot.failure_reason = doc.HasMember("failure_reason") && doc["failure_reason"].IsString()
                                  ? doc["failure_reason"].GetString()
                                  : "";
    snapshot.failure_reason_detail =
        doc.HasMember("failure_reason_detail") && doc["failure_reason_detail"].IsString()
            ? doc["failure_reason_detail"].GetString()
            : "";
    *out = std::move(snapshot);
    if (out_json) *out_json = std::move(json);
    return true;
}

bool ReadTaskTransformName(IBaselineValueTask* task,
                           std::string* out_transform_name,
                           std::string* err) {
    if (!task || !out_transform_name) return false;
    std::string json;
    const int rc = task->QueryTaskSnapshotJson(&json);
    if (rc != error::OK) {
        if (err) *err = "QueryTaskSnapshotJson failed: " + std::to_string(rc);
        return false;
    }
    rapidjson::Document doc;
    doc.Parse(json.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        if (err) *err = "task snapshot json parse failed";
        return false;
    }
    if (!doc.HasMember("transform_name") || !doc["transform_name"].IsString()) {
        if (err) *err = "task snapshot lacks transform_name";
        return false;
    }
    *out_transform_name = doc["transform_name"].GetString();
    return true;
}

TraceRow BuildTraceRow(const std::string& phase,
                       const SeriesPoint& point,
                       const DetectorResult& result,
                       const std::string& transform_name) {
    TraceRow row;
    row.phase = phase;
    row.time_text = point.time_text;
    row.bucket_id = point.bucket_id;
    row.actual_value = point.value;
    row.normalized_score = result.normalized_score;
    row.confidence = result.confidence;
    row.persistence = result.persistence;
    row.provider = result.provider;
    row.reason = result.reason;
    row.direction = result.direction;
    row.flags = result.flags;

    if (result.evidence.kind == BaselineEvidenceKind::kValue) {
        const ValueEvidence& evidence = result.evidence.value;
        row.baseline_internal = evidence.baseline_mu_t;
        row.baseline_value = InverseTransformValue(transform_name, evidence.baseline_mu_t);
        row.has_baseline = evidence.model_state != BaselineModelState::kUnknown ||
                           evidence.shadow_active ||
                           result.provider != BaselineProvider::kNone;
        row.residual = evidence.resid_r_t;
        row.z_score = evidence.z_t;
        row.p_shift = evidence.p_shift_t;
        row.score_point = evidence.score_point;
        row.score_shift = evidence.score_shift;
        row.shadow_active = evidence.shadow_active;
    }
    return row;
}

void AddTriggerPoint(std::vector<TriggerPoint>* out,
                     const std::string& trigger_type,
                     const TraceRow& row,
                     const std::string& detail) {
    if (!out) return;
    TriggerPoint trigger;
    trigger.trigger_type = trigger_type;
    trigger.time_text = row.time_text;
    trigger.bucket_id = row.bucket_id;
    trigger.actual_value = row.actual_value;
    trigger.has_baseline = row.has_baseline;
    trigger.baseline_value = row.baseline_value;
    trigger.score_point = row.score_point;
    trigger.score_shift = row.score_shift;
    trigger.normalized_score = row.normalized_score;
    trigger.provider = ProviderName(row.provider);
    trigger.reason = ReasonName(row.reason);
    trigger.flags = FlagsToString(row.flags);
    trigger.detail = detail;
    out->push_back(std::move(trigger));
}

void AddBootstrapTrigger(std::vector<TriggerPoint>* out,
                         const SeriesPoint& point,
                         uint64_t formal_model_version) {
    if (!out) return;
    TriggerPoint trigger;
    trigger.trigger_type = "bootstrap_formal_ready";
    trigger.time_text = point.time_text;
    trigger.bucket_id = point.bucket_id;
    trigger.actual_value = point.value;
    trigger.provider = "formal";
    trigger.detail = "formal_model_version=" + std::to_string(formal_model_version);
    out->push_back(std::move(trigger));
}

double Quantile(std::vector<double> values, double q) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double pos = q * static_cast<double>(values.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(std::floor(pos));
    const std::size_t hi = static_cast<std::size_t>(std::ceil(pos));
    if (lo == hi) return values[lo];
    const double frac = pos - static_cast<double>(lo);
    return values[lo] * (1.0 - frac) + values[hi] * frac;
}

void MaybeAppendScoredPoint(const SeriesPoint& point,
                            const DetectorResult& result,
                            ReplaySummary* summary) {
    if (!summary) return;
    if (result.normalized_score <= 0.0 &&
        (result.flags & (kBaselineFlagRebuildQueued | kBaselineFlagShadowActive)) == 0) {
        return;
    }
    ScoredPoint scored;
    scored.time_text = point.time_text;
    scored.bucket_id = point.bucket_id;
    scored.value = point.value;
    scored.normalized_score = result.normalized_score;
    scored.confidence = result.confidence;
    scored.persistence = result.persistence;
    scored.provider = result.provider;
    scored.reason = result.reason;
    scored.direction = result.direction;
    scored.flags = result.flags;
    summary->scored_points.push_back(std::move(scored));
}

std::vector<ScoredPoint> SelectTopScores(const std::vector<ScoredPoint>& points, int topk) {
    std::vector<ScoredPoint> top = points;
    std::sort(top.begin(), top.end(), [](const ScoredPoint& lhs, const ScoredPoint& rhs) {
        if (lhs.normalized_score != rhs.normalized_score) {
            return lhs.normalized_score > rhs.normalized_score;
        }
        return lhs.bucket_id < rhs.bucket_id;
    });
    if (topk > 0 && static_cast<std::size_t>(topk) < top.size()) {
        top.resize(static_cast<std::size_t>(topk));
    }
    return top;
}

std::vector<CapabilityItem> BuildCapabilities(const ReplaySummary& summary) {
    std::vector<CapabilityItem> items;

    CapabilityItem cold_start;
    cold_start.name = "cold_start";
    cold_start.observed = summary.cold_start_count > 0;
    cold_start.detail = "cold_start flags=" + std::to_string(summary.cold_start_count);
    items.push_back(std::move(cold_start));

    CapabilityItem bootstrap;
    bootstrap.name = "bootstrap_formal_rebuild";
    bootstrap.observed = summary.bootstrap_formal_ready;
    bootstrap.detail = "formal_version=" +
                       std::to_string(summary.bootstrap_formal_model_version) +
                       ", history_fetch_count=" +
                       std::to_string(summary.history_fetch_count);
    items.push_back(std::move(bootstrap));

    CapabilityItem pre_shift;
    pre_shift.name = "pre_shift_formal_stability";
    pre_shift.observed = !summary.pre_shift_formal_scores.empty();
    pre_shift.detail = "formal_score_p50=" +
                       std::to_string(Quantile(summary.pre_shift_formal_scores, 0.50)) +
                       ", formal_score_p95=" +
                       std::to_string(Quantile(summary.pre_shift_formal_scores, 0.95));
    items.push_back(std::move(pre_shift));

    CapabilityItem point_anomaly;
    point_anomaly.name = "point_anomaly_detection";
    point_anomaly.observed = summary.positive_score_count > 0;
    point_anomaly.detail = "positive_scores=" +
                           std::to_string(summary.positive_score_count) +
                           ", medium_or_higher=" +
                           std::to_string(summary.medium_or_higher_count) +
                           ", high=" + std::to_string(summary.high_severity_count);
    items.push_back(std::move(point_anomaly));

    CapabilityItem shift;
    shift.name = "level_shift_detection";
    shift.observed = summary.saw_rebuild_queued;
    shift.detail = summary.saw_rebuild_queued
                       ? ("first_rebuild_queued_time=" + summary.first_rebuild_queued_time)
                       : "rebuild_queued not observed";
    items.push_back(std::move(shift));

    CapabilityItem shadow;
    shadow.name = "shadow_bridge";
    shadow.observed = summary.saw_shadow_active;
    shadow.detail = summary.saw_shadow_active
                        ? ("first_shadow_time=" + summary.first_shadow_time)
                        : "shadow_active not observed";
    items.push_back(std::move(shadow));

    CapabilityItem switch_item;
    switch_item.name = "formal_switch";
    switch_item.observed = summary.saw_formal_switch;
    switch_item.detail = summary.saw_formal_switch
                             ? ("first_switch_time=" + summary.first_switch_time +
                                ", final_formal_version=" +
                                std::to_string(summary.final_snapshot.formal_model_version))
                             : ("final_formal_version=" +
                                std::to_string(summary.final_snapshot.formal_model_version) +
                                ", shadow_active=" +
                                std::string(summary.final_snapshot.shadow_active ? "true" : "false") +
                                ", candidate_state=" + summary.final_snapshot.candidate_state +
                                ", switch_state=" + summary.final_snapshot.switch_state +
                                ", failure_reason=" + summary.final_snapshot.failure_reason);
    items.push_back(std::move(switch_item));

    return items;
}

bool EnsureDirectory(const std::string& path, std::string* err) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return true;
    if (std::filesystem::create_directories(path, ec)) return true;
    if (err) *err = "failed to create directory: " + path + ", ec=" + std::to_string(ec.value());
    return false;
}

std::vector<EventWindow> BuildEventWindows(const std::vector<TraceRow>& trace_rows) {
    std::vector<EventWindow> windows;
    const auto close_window = [&windows](const std::vector<TraceRow>& rows,
                                         std::size_t begin,
                                         std::size_t end) {
        if (begin >= end) return;
        EventWindow window;
        window.start_time = rows[begin].time_text;
        window.end_time = rows[end - 1].time_text;
        window.start_bucket = rows[begin].bucket_id;
        window.end_bucket = rows[end - 1].bucket_id;
        window.point_count = end - begin;
        window.start_reason = ReasonName(rows[begin].reason);
        window.start_flags = FlagsToString(rows[begin].flags);
        window.event_type = "point";
        std::size_t peak_index = begin;
        for (std::size_t i = begin; i < end; ++i) {
            const TraceRow& row = rows[i];
            if (row.score_shift > 0.0 || (row.flags & kBaselineFlagRebuildQueued) != 0) {
                window.event_type = "shift";
            }
            if ((row.flags & kBaselineFlagShadowActive) != 0) {
                window.shadow_involved = true;
            }
            if ((row.flags & kBaselineFlagRebuildQueued) != 0) {
                window.rebuild_queued = true;
            }
            if (row.normalized_score > rows[peak_index].normalized_score ||
                (row.normalized_score == rows[peak_index].normalized_score &&
                 row.bucket_id < rows[peak_index].bucket_id)) {
                peak_index = i;
            }
        }
        const TraceRow& peak = rows[peak_index];
        window.peak_time = peak.time_text;
        window.peak_bucket = peak.bucket_id;
        window.peak_value = peak.actual_value;
        window.peak_has_baseline = peak.has_baseline;
        window.peak_baseline = peak.baseline_value;
        window.peak_score_point = peak.score_point;
        window.peak_score_shift = peak.score_shift;
        window.peak_normalized_score = peak.normalized_score;
        windows.push_back(std::move(window));
    };

    bool in_window = false;
    std::size_t window_begin = 0;
    int64_t previous_bucket = 0;
    for (std::size_t i = 0; i < trace_rows.size(); ++i) {
        const TraceRow& row = trace_rows[i];
        const bool active = IsTraceEventActive(row);
        if (!active) {
            if (in_window) {
                close_window(trace_rows, window_begin, i);
                in_window = false;
            }
            continue;
        }
        if (!in_window) {
            in_window = true;
            window_begin = i;
        } else if (row.bucket_id != previous_bucket + 1) {
            close_window(trace_rows, window_begin, i);
            window_begin = i;
        }
        previous_bucket = row.bucket_id;
    }
    if (in_window) {
        close_window(trace_rows, window_begin, trace_rows.size());
    }
    return windows;
}

void AppendWindowStartTriggers(const std::vector<EventWindow>& windows,
                               const std::vector<TraceRow>& rows,
                               std::vector<TriggerPoint>* out) {
    if (!out) return;
    for (const auto& window : windows) {
        auto it = std::find_if(rows.begin(), rows.end(), [&window](const TraceRow& row) {
            return row.bucket_id == window.start_bucket;
        });
        if (it == rows.end()) continue;
        AddTriggerPoint(out,
                        "event_window_start",
                        *it,
                        "event_type=" + window.event_type +
                            ", point_count=" + std::to_string(window.point_count));
    }
}

bool WriteTraceCsv(const std::string& path,
                   const std::vector<TraceRow>& rows,
                   std::string* err) {
    std::ofstream output(path);
    if (!output.is_open()) {
        if (err) *err = "failed to open baseline trace csv: " + path;
        return false;
    }
    output << "phase,time,bucket_id,actual_value,baseline_value,baseline_internal,residual,z_score,p_shift,score_point,score_shift,normalized_score,confidence,persistence,provider,reason,direction,flags,shadow_active\n";
    for (const auto& row : rows) {
        output << CsvEscape(row.phase) << ','
               << CsvEscape(row.time_text) << ','
               << row.bucket_id << ','
               << row.actual_value << ',';
        if (row.has_baseline) {
            output << row.baseline_value << ',' << row.baseline_internal;
        } else {
            output << ',';
        }
        output << ',' << row.residual
               << ',' << row.z_score
               << ',' << row.p_shift
               << ',' << row.score_point
               << ',' << row.score_shift
               << ',' << row.normalized_score
               << ',' << row.confidence
               << ',' << row.persistence
               << ',' << CsvEscape(ProviderName(row.provider))
               << ',' << CsvEscape(ReasonName(row.reason))
               << ',' << CsvEscape(DirectionName(row.direction))
               << ',' << CsvEscape(FlagsToString(row.flags))
               << ',' << CsvEscape(row.shadow_active ? "true" : "false")
               << '\n';
    }
    return true;
}

bool WriteTriggerPointsCsv(const std::string& path,
                           const std::vector<TriggerPoint>& triggers,
                           std::string* err) {
    std::ofstream output(path);
    if (!output.is_open()) {
        if (err) *err = "failed to open trigger points csv: " + path;
        return false;
    }
    output << "trigger_type,time,bucket_id,actual_value,baseline_value,score_point,score_shift,normalized_score,provider,reason,flags,detail\n";
    for (const auto& trigger : triggers) {
        output << CsvEscape(trigger.trigger_type) << ','
               << CsvEscape(trigger.time_text) << ','
               << trigger.bucket_id << ','
               << trigger.actual_value << ',';
        if (trigger.has_baseline) {
            output << trigger.baseline_value;
        }
        output << ',' << trigger.score_point
               << ',' << trigger.score_shift
               << ',' << trigger.normalized_score
               << ',' << CsvEscape(trigger.provider)
               << ',' << CsvEscape(trigger.reason)
               << ',' << CsvEscape(trigger.flags)
               << ',' << CsvEscape(trigger.detail)
               << '\n';
    }
    return true;
}

bool WriteEventWindowsCsv(const std::string& path,
                          const std::vector<EventWindow>& windows,
                          std::string* err) {
    std::ofstream output(path);
    if (!output.is_open()) {
        if (err) *err = "failed to open event windows csv: " + path;
        return false;
    }
    output << "event_type,start_time,end_time,start_bucket,end_bucket,point_count,start_reason,start_flags,shadow_involved,rebuild_queued,peak_time,peak_bucket,peak_value,peak_baseline,peak_score_point,peak_score_shift,peak_normalized_score\n";
    for (const auto& window : windows) {
        output << CsvEscape(window.event_type) << ','
               << CsvEscape(window.start_time) << ','
               << CsvEscape(window.end_time) << ','
               << window.start_bucket << ','
               << window.end_bucket << ','
               << window.point_count << ','
               << CsvEscape(window.start_reason) << ','
               << CsvEscape(window.start_flags) << ','
               << CsvEscape(window.shadow_involved ? "true" : "false") << ','
               << CsvEscape(window.rebuild_queued ? "true" : "false") << ','
               << CsvEscape(window.peak_time) << ','
               << window.peak_bucket << ','
               << window.peak_value << ',';
        if (window.peak_has_baseline) {
            output << window.peak_baseline;
        }
        output << ',' << window.peak_score_point
               << ',' << window.peak_score_shift
               << ',' << window.peak_normalized_score
               << '\n';
    }
    return true;
}

bool WriteTopkCsv(const std::string& path, const std::vector<ScoredPoint>& topk, std::string* err) {
    std::ofstream output(path);
    if (!output.is_open()) {
        if (err) *err = "failed to open csv report: " + path;
        return false;
    }
    output << "rank,time,bucket_id,value,normalized_score,confidence,persistence,provider,reason,direction,flags\n";
    for (std::size_t i = 0; i < topk.size(); ++i) {
        const auto& row = topk[i];
        output << (i + 1) << ','
               << CsvEscape(row.time_text) << ','
               << row.bucket_id << ','
               << row.value << ','
               << row.normalized_score << ','
               << row.confidence << ','
               << row.persistence << ','
               << CsvEscape(ProviderName(row.provider)) << ','
               << CsvEscape(ReasonName(row.reason)) << ','
               << CsvEscape(DirectionName(row.direction)) << ','
               << CsvEscape(FlagsToString(row.flags)) << '\n';
    }
    return true;
}

bool WriteReplayReport(const CliOptions& options,
                       const ReplaySummary& summary,
                       const std::vector<ScoredPoint>& topk,
                       const std::string& json_path,
                       std::string* err) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

    const std::vector<CapabilityItem> capabilities = BuildCapabilities(summary);

    writer.StartObject();
    writer.Key("mode");
    writer.String(options.mode.c_str());
    writer.Key("csv_path");
    writer.String(options.csv_path.c_str());
    writer.Key("task_config");
    writer.StartObject();
    writer.Key("task_name");
    writer.String(options.task_name.c_str());
    writer.Key("key_field");
    writer.String(options.key_field.c_str());
    writer.Key("series_key");
    writer.String(options.series_key.c_str());
    writer.Key("feature");
    writer.String(options.feature.c_str());
    writer.Key("feature_profile");
    writer.String(options.feature_profile.c_str());
    writer.Key("feature_type");
    writer.String(options.feature_type.c_str());
    writer.Key("tz");
    writer.String(options.tz.c_str());
    writer.Key("delta");
    writer.Int64(options.delta);
    writer.Key("bootstrap_days");
    writer.Int(options.bootstrap_days);
    writer.EndObject();

    writer.Key("dataset");
    writer.StartObject();
    writer.Key("start_time");
    writer.String(summary.start_time.c_str());
    writer.Key("end_time");
    writer.String(summary.end_time.c_str());
    writer.Key("point_count");
    writer.Uint64(summary.total_points);
    writer.EndObject();

    writer.Key("summary");
    writer.StartObject();
    writer.Key("bootstrap_rebuild_requested");
    writer.Bool(summary.bootstrap_rebuild_requested);
    writer.Key("bootstrap_formal_ready");
    writer.Bool(summary.bootstrap_formal_ready);
    writer.Key("bootstrap_bucket_id");
    writer.Int64(summary.bootstrap_bucket_id);
    writer.Key("bootstrap_formal_model_version");
    writer.Uint64(summary.bootstrap_formal_model_version);
    writer.Key("cold_start_count");
    writer.Uint64(summary.cold_start_count);
    writer.Key("positive_score_count");
    writer.Uint64(summary.positive_score_count);
    writer.Key("medium_or_higher_count");
    writer.Uint64(summary.medium_or_higher_count);
    writer.Key("high_severity_count");
    writer.Uint64(summary.high_severity_count);
    writer.Key("rebuild_queued_count");
    writer.Uint64(summary.rebuild_queued_count);
    writer.Key("shadow_result_count");
    writer.Uint64(summary.shadow_result_count);
    writer.Key("history_fetch_count");
    writer.Uint64(summary.history_fetch_count);
    writer.Key("trace_row_count");
    writer.Uint64(summary.trace_rows.size());
    writer.Key("trigger_point_count");
    writer.Uint64(summary.trigger_points.size());
    writer.Key("event_window_count");
    writer.Uint64(summary.event_windows.size());
    writer.Key("first_rebuild_queued_time");
    writer.String(summary.first_rebuild_queued_time.c_str());
    writer.Key("first_shadow_time");
    writer.String(summary.first_shadow_time.c_str());
    writer.Key("first_switch_time");
    writer.String(summary.first_switch_time.c_str());
    writer.Key("pre_shift_formal_score_p50");
    writer.Double(Quantile(summary.pre_shift_formal_scores, 0.50));
    writer.Key("pre_shift_formal_score_p95");
    writer.Double(Quantile(summary.pre_shift_formal_scores, 0.95));
    writer.Key("final_snapshot");
    writer.StartObject();
    writer.Key("formal_ready");
    writer.Bool(summary.final_snapshot.formal_ready);
    writer.Key("shadow_active");
    writer.Bool(summary.final_snapshot.shadow_active);
    writer.Key("formal_model_version");
    writer.Uint64(summary.final_snapshot.formal_model_version);
    writer.Key("model_state");
    writer.String(summary.final_snapshot.model_state.c_str());
    writer.Key("switch_state");
    writer.String(summary.final_snapshot.switch_state.c_str());
    writer.Key("candidate_state");
    writer.String(summary.final_snapshot.candidate_state.c_str());
    writer.Key("drift_direction");
    writer.String(summary.final_snapshot.drift_direction.c_str());
    writer.Key("failure_reason");
    writer.String(summary.final_snapshot.failure_reason.c_str());
    writer.Key("failure_reason_detail");
    writer.String(summary.final_snapshot.failure_reason_detail.c_str());
    writer.EndObject();
    writer.EndObject();

    writer.Key("capabilities");
    writer.StartArray();
    for (const auto& item : capabilities) {
        writer.StartObject();
        writer.Key("name");
        writer.String(item.name.c_str());
        writer.Key("observed");
        writer.Bool(item.observed);
        writer.Key("detail");
        writer.String(item.detail.c_str());
        writer.EndObject();
    }
    writer.EndArray();

    writer.Key("topk");
    writer.StartArray();
    for (const auto& row : topk) {
        writer.StartObject();
        writer.Key("time");
        writer.String(row.time_text.c_str());
        writer.Key("bucket_id");
        writer.Int64(row.bucket_id);
        writer.Key("value");
        writer.Double(row.value);
        writer.Key("normalized_score");
        writer.Double(row.normalized_score);
        writer.Key("confidence");
        writer.Double(row.confidence);
        writer.Key("persistence");
        writer.Uint(row.persistence);
        writer.Key("provider");
        writer.String(ProviderName(row.provider));
        writer.Key("reason");
        writer.String(ReasonName(row.reason));
        writer.Key("direction");
        writer.String(DirectionName(row.direction));
        writer.Key("flags");
        writer.String(FlagsToString(row.flags).c_str());
        writer.EndObject();
    }
    writer.EndArray();

    writer.Key("bootstrap_snapshot_json");
    writer.String(summary.bootstrap_snapshot_json.c_str());
    writer.Key("final_snapshot_json");
    writer.String(summary.final_snapshot_json.c_str());
    writer.EndObject();

    std::ofstream output(json_path);
    if (!output.is_open()) {
        if (err) *err = "failed to open json report: " + json_path;
        return false;
    }
    output << buffer.GetString() << '\n';
    return true;
}

bool WaitForFormalReady(IBaselineValueTask* task,
                        const std::string& series_key,
                        SnapshotState* out_snapshot,
                        std::string* out_json) {
    return WaitUntil([&]() {
        SnapshotState snapshot;
        std::string json;
        if (!ReadSnapshot(task, series_key, &snapshot, &json, nullptr)) return false;
        if (snapshot.formal_ready) {
            if (out_snapshot) *out_snapshot = snapshot;
            if (out_json) *out_json = json;
            return true;
        }
        return false;
    }, 15000);
}

bool RunReplay(const CliOptions& options) {
    std::vector<SeriesPoint> points;
    std::string err;
    if (!LoadCsvSeries(options, &points, &err)) {
        std::fprintf(stderr, "[ERROR] %s\n", err.c_str());
        return false;
    }

    ReplaySummary summary;
    summary.total_points = points.size();
    summary.start_time = points.front().time_text;
    summary.end_time = points.back().time_text;

    const int64_t bootstrap_points =
        std::max<int64_t>(1, static_cast<int64_t>(options.bootstrap_days) *
                                 (kSecondsPerDay / options.delta));
    const std::size_t warmup_count =
        static_cast<std::size_t>(std::min<int64_t>(bootstrap_points, points.size()));
    summary.bootstrap_bucket_id = points[warmup_count - 1].bucket_id;

    std::printf("[INFO] loaded %zu points from %s\n",
                points.size(),
                options.csv_path.c_str());
    std::printf("[INFO] time range: %s -> %s\n",
                summary.start_time.c_str(),
                summary.end_time.c_str());
    std::printf("[INFO] bootstrap_days=%d, warmup_points=%zu, delta=%lld, tz=%s\n",
                options.bootstrap_days,
                warmup_count,
                static_cast<long long>(options.delta),
                options.tz.c_str());

    auto env = LoadBaselineService();
    auto* service = env.service;
    const std::string task_config = BuildValueTaskConfigJson(options);
    IBaselineValueTask* task = nullptr;
    const int create_rc = service->CreateValueTask(task_config.c_str(), &task);
    if (create_rc != error::OK || task == nullptr) {
        std::fprintf(stderr, "[ERROR] CreateValueTask failed: rc=%d\n", create_rc);
        return false;
    }

    VectorValueHistoryReader reader(options.series_key, &points);
    const int bind_rc = task->SetHistoryReader(&reader);
    if (bind_rc != error::OK) {
        std::fprintf(stderr, "[ERROR] SetHistoryReader failed: rc=%d\n", bind_rc);
        return false;
    }
    std::string transform_name = "log1p";
    if (!ReadTaskTransformName(task, &transform_name, &err)) {
        std::fprintf(stderr,
                     "[WARN] failed to read task transform_name, fallback to log1p: %s\n",
                     err.c_str());
        transform_name = "log1p";
    }

    DetectorResult result{};
    for (std::size_t i = 0; i < warmup_count; ++i) {
        const ValueObservation obs{
            StringRef(options.series_key),
            points[i].bucket_id,
            points[i].value,
            points[i].sample_count,
        };
        const int submit_rc = task->SubmitObservation(obs, &result);
        if (submit_rc != error::OK) {
            std::fprintf(stderr,
                         "[ERROR] SubmitObservation failed during warmup at idx=%zu rc=%d\n",
                         i,
                         submit_rc);
            return false;
        }
        if ((result.flags & kBaselineFlagColdStart) != 0) ++summary.cold_start_count;
        summary.trace_rows.push_back(
            BuildTraceRow("warmup", points[i], result, transform_name));
    }

    summary.bootstrap_rebuild_requested = true;
    const int rebuild_rc =
        task->RequestRebuild(StringRef(options.series_key), BaselineRebuildReason::kManual);
    if (rebuild_rc != error::OK) {
        std::fprintf(stderr, "[ERROR] RequestRebuild failed: rc=%d\n", rebuild_rc);
        return false;
    }

    if (!WaitForFormalReady(task,
                            options.series_key,
                            &summary.bootstrap_snapshot,
                            &summary.bootstrap_snapshot_json)) {
        std::fprintf(stderr, "[ERROR] bootstrap formal model was not ready in time\n");
        return false;
    }
    summary.bootstrap_formal_ready = true;
    summary.bootstrap_formal_model_version =
        summary.bootstrap_snapshot.formal_model_version;
    summary.history_fetch_count = reader.fetch_count();
    AddBootstrapTrigger(&summary.trigger_points,
                        points[warmup_count - 1],
                        summary.bootstrap_formal_model_version);

    std::printf("[INFO] bootstrap formal ready, version=%llu, history_fetch_count=%zu\n",
                static_cast<unsigned long long>(summary.bootstrap_formal_model_version),
                summary.history_fetch_count);

    for (std::size_t i = warmup_count; i < points.size(); ++i) {
        const ValueObservation obs{
            StringRef(options.series_key),
            points[i].bucket_id,
            points[i].value,
            points[i].sample_count,
        };
        DetectorResult current{};
        const int submit_rc = task->SubmitObservation(obs, &current);
        if (submit_rc != error::OK) {
            std::fprintf(stderr,
                         "[ERROR] SubmitObservation failed at idx=%zu rc=%d\n",
                         i,
                         submit_rc);
            return false;
        }

        if ((current.flags & kBaselineFlagColdStart) != 0) ++summary.cold_start_count;
        if (current.normalized_score > 0.0) ++summary.positive_score_count;
        if (current.normalized_score >= 0.50) ++summary.medium_or_higher_count;
        if (current.normalized_score >= 0.85) ++summary.high_severity_count;
        if ((current.flags & kBaselineFlagRebuildQueued) != 0) ++summary.rebuild_queued_count;
        if ((current.flags & kBaselineFlagShadowActive) != 0) ++summary.shadow_result_count;

        const TraceRow trace_row = BuildTraceRow("replay", points[i], current, transform_name);
        summary.trace_rows.push_back(trace_row);
        MaybeAppendScoredPoint(points[i], current, &summary);

        if (!summary.saw_rebuild_queued &&
            (current.flags & kBaselineFlagRebuildQueued) != 0) {
            summary.saw_rebuild_queued = true;
            summary.first_rebuild_queued_time = points[i].time_text;
            summary.first_rebuild_queued_bucket = points[i].bucket_id;
            AddTriggerPoint(&summary.trigger_points,
                            "rebuild_queued_first",
                            trace_row,
                            "first rebuild queued by shift detection");
        }
        if (!summary.saw_shadow_active &&
            (current.flags & kBaselineFlagShadowActive) != 0) {
            summary.saw_shadow_active = true;
            summary.first_shadow_time = points[i].time_text;
            summary.first_shadow_bucket = points[i].bucket_id;
            AddTriggerPoint(&summary.trigger_points,
                            "shadow_active_first",
                            trace_row,
                            "first shadow-active scoring row");
        }

        if (!summary.saw_rebuild_queued && current.provider == BaselineProvider::kFormal) {
            summary.pre_shift_formal_scores.push_back(current.normalized_score);
        }

        const bool need_poll_snapshot =
            ((current.flags & (kBaselineFlagRebuildQueued | kBaselineFlagShadowActive)) != 0) ||
            (summary.saw_rebuild_queued && !summary.saw_formal_switch &&
             (i % kSnapshotPollInterval == 0));
        if (need_poll_snapshot) {
            SnapshotState snapshot;
            std::string snapshot_json;
            if (!ReadSnapshot(task, options.series_key, &snapshot, &snapshot_json, nullptr)) {
                continue;
            }
            if (snapshot.shadow_active && !summary.saw_shadow_active) {
                summary.saw_shadow_active = true;
                summary.first_shadow_time = points[i].time_text;
                summary.first_shadow_bucket = points[i].bucket_id;
            }
            if (!summary.saw_formal_switch &&
                snapshot.formal_model_version >
                    summary.bootstrap_formal_model_version &&
                !snapshot.shadow_active) {
                summary.saw_formal_switch = true;
                summary.first_switch_time = points[i].time_text;
                summary.first_switch_bucket = points[i].bucket_id;
            }
        }
    }

    summary.history_fetch_count = reader.fetch_count();
    if (!ReadSnapshot(task,
                      options.series_key,
                      &summary.final_snapshot,
                      &summary.final_snapshot_json,
                      &err)) {
        std::fprintf(stderr, "[ERROR] final snapshot read failed: %s\n", err.c_str());
        return false;
    }

    if (!summary.saw_formal_switch &&
        summary.final_snapshot.formal_model_version >
            summary.bootstrap_formal_model_version &&
        !summary.final_snapshot.shadow_active) {
        summary.saw_formal_switch = true;
        summary.first_switch_time = summary.end_time;
        summary.first_switch_bucket = points.back().bucket_id;
    }

    if (summary.final_snapshot.shadow_active && !summary.saw_formal_switch) {
        std::printf("[INFO] final snapshot still in shadow, requesting a manual rebuild for switch validation\n");
        summary.manual_switch_validation_requested = true;
        if (!summary.trace_rows.empty()) {
            AddTriggerPoint(&summary.trigger_points,
                            "manual_switch_validation_rebuild_requested",
                            summary.trace_rows.back(),
                            "post-replay manual rebuild to validate formal switch");
        }
        const int manual_switch_rebuild_rc =
            task->RequestRebuild(StringRef(options.series_key), BaselineRebuildReason::kManual);
        if (manual_switch_rebuild_rc != error::OK) {
            std::fprintf(stderr,
                         "[WARN] manual switch validation rebuild request failed: rc=%d\n",
                         manual_switch_rebuild_rc);
        }
    }

    if (summary.saw_shadow_active && !summary.saw_formal_switch) {
        WaitUntil([&]() {
            SnapshotState snapshot;
            std::string snapshot_json;
            if (!ReadSnapshot(task, options.series_key, &snapshot, &snapshot_json, nullptr)) {
                return false;
            }
            summary.final_snapshot = snapshot;
            summary.final_snapshot_json = snapshot_json;
            if (snapshot.formal_model_version > summary.bootstrap_formal_model_version &&
                !snapshot.shadow_active) {
                summary.saw_formal_switch = true;
                summary.first_switch_time =
                    summary.end_time + " (post_replay_manual_rebuild)";
                summary.first_switch_bucket = points.back().bucket_id;
                return true;
            }
            return false;
        }, 30000);
    }

    summary.history_fetch_count = reader.fetch_count();
    summary.event_windows = BuildEventWindows(summary.trace_rows);
    AppendWindowStartTriggers(summary.event_windows,
                              summary.trace_rows,
                              &summary.trigger_points);

    if (!summary.trace_rows.empty()) {
        if (summary.saw_formal_switch) {
            AddTriggerPoint(&summary.trigger_points,
                            "formal_switch_applied",
                            summary.trace_rows.back(),
                            "formal model switch completed");
        } else if (!summary.final_snapshot.failure_reason.empty() &&
                   summary.final_snapshot.failure_reason != "none") {
            AddTriggerPoint(&summary.trigger_points,
                            "final_rebuild_outcome",
                            summary.trace_rows.back(),
                            "candidate_state=" + summary.final_snapshot.candidate_state +
                                ", switch_state=" + summary.final_snapshot.switch_state +
                                ", failure_reason=" + summary.final_snapshot.failure_reason);
        }
    }

    if (!EnsureDirectory(options.report_dir, &err)) {
        std::fprintf(stderr, "[ERROR] %s\n", err.c_str());
        return false;
    }

    const std::vector<ScoredPoint> topk =
        SelectTopScores(summary.scored_points, options.topk);
    const std::string baseline_trace_csv =
        options.report_dir + "/baseline_trace.csv";
    const std::string trigger_points_csv =
        options.report_dir + "/trigger_points.csv";
    const std::string event_windows_csv =
        options.report_dir + "/event_windows.csv";
    const std::string topk_csv =
        options.report_dir + "/baseline_value_replay_topk.csv";
    const std::string report_json =
        options.report_dir + "/baseline_value_replay_report.json";

    if (!WriteTraceCsv(baseline_trace_csv, summary.trace_rows, &err)) {
        std::fprintf(stderr, "[ERROR] %s\n", err.c_str());
        return false;
    }
    if (!WriteTriggerPointsCsv(trigger_points_csv, summary.trigger_points, &err)) {
        std::fprintf(stderr, "[ERROR] %s\n", err.c_str());
        return false;
    }
    if (!WriteEventWindowsCsv(event_windows_csv, summary.event_windows, &err)) {
        std::fprintf(stderr, "[ERROR] %s\n", err.c_str());
        return false;
    }
    if (!WriteTopkCsv(topk_csv, topk, &err)) {
        std::fprintf(stderr, "[ERROR] %s\n", err.c_str());
        return false;
    }
    if (!WriteReplayReport(options, summary, topk, report_json, &err)) {
        std::fprintf(stderr, "[ERROR] %s\n", err.c_str());
        return false;
    }

    std::printf("[CAPABILITY] cold_start=%s, bootstrap_formal=%s, anomaly_detection=%s\n",
                summary.cold_start_count > 0 ? "yes" : "no",
                summary.bootstrap_formal_ready ? "yes" : "no",
                summary.positive_score_count > 0 ? "yes" : "no");
    std::printf("[CAPABILITY] level_shift=%s, shadow_bridge=%s, formal_switch=%s\n",
                summary.saw_rebuild_queued ? "yes" : "no",
                summary.saw_shadow_active ? "yes" : "no",
                summary.saw_formal_switch ? "yes" : "no");
    std::printf("[FINAL] candidate_state=%s, switch_state=%s, failure_reason=%s, failure_detail=%s\n",
                summary.final_snapshot.candidate_state.c_str(),
                summary.final_snapshot.switch_state.c_str(),
                summary.final_snapshot.failure_reason.c_str(),
                summary.final_snapshot.failure_reason_detail.c_str());
    std::printf("[METRIC] pre_shift_formal_score_p50=%.6f, p95=%.6f\n",
                Quantile(summary.pre_shift_formal_scores, 0.50),
                Quantile(summary.pre_shift_formal_scores, 0.95));
    if (!summary.first_rebuild_queued_time.empty()) {
        std::printf("[EVENT] first_rebuild_queued=%s\n",
                    summary.first_rebuild_queued_time.c_str());
    }
    if (!summary.first_shadow_time.empty()) {
        std::printf("[EVENT] first_shadow=%s\n", summary.first_shadow_time.c_str());
    }
    if (!summary.first_switch_time.empty()) {
        std::printf("[EVENT] first_formal_switch=%s\n", summary.first_switch_time.c_str());
    }
    std::printf("[OUTPUT] %s\n", report_json.c_str());
    std::printf("[OUTPUT] %s\n", baseline_trace_csv.c_str());
    std::printf("[OUTPUT] %s\n", trigger_points_csv.c_str());
    std::printf("[OUTPUT] %s\n", event_windows_csv.c_str());
    std::printf("[OUTPUT] %s\n", topk_csv.c_str());
    return true;
}

std::vector<SeriesPoint> BuildDemoSeries(int64_t delta) {
    std::vector<SeriesPoint> points;
    points.reserve(7 * 24);
    const int64_t start_bucket = static_cast<int64_t>(1720000000) / delta;
    constexpr double kPi = 3.14159265358979323846;
    for (int i = 0; i < 7 * 24; ++i) {
        const double daily_phase =
            2.0 * kPi * static_cast<double>(i % 24) / 24.0;
        const double weekly_phase =
            2.0 * kPi * static_cast<double>(i) / static_cast<double>(7 * 24);
        SeriesPoint point;
        point.time_text = "demo_" + std::to_string(i);
        point.bucket_id = start_bucket + i;
        point.value = 100.0 + 18.0 * std::sin(daily_phase) + 6.0 * std::cos(weekly_phase);
        points.push_back(std::move(point));
    }
    return points;
}

bool RunDemo(const CliOptions& options) {
    std::vector<SeriesPoint> demo_points = BuildDemoSeries(3600);

    CliOptions demo_options = options;
    demo_options.delta = 3600;
    demo_options.task_name = "demo_value_task";
    demo_options.key_field = "service";
    demo_options.series_key = "svc-demo";
    demo_options.feature = "bps_mbps";
    demo_options.feature_profile = "traffic";
    demo_options.feature_type = "value_basic";
    demo_options.tz = "UTC";

    auto env = LoadBaselineService();
    auto* service = env.service;
    const std::string task_config = BuildValueTaskConfigJson(demo_options);
    IBaselineValueTask* task = nullptr;
    const int create_rc = service->CreateValueTask(task_config.c_str(), &task);
    if (create_rc != error::OK || task == nullptr) {
        std::fprintf(stderr, "[ERROR] demo CreateValueTask failed: rc=%d\n", create_rc);
        return false;
    }

    VectorValueHistoryReader reader(demo_options.series_key, &demo_points);
    const int bind_rc = task->SetHistoryReader(&reader);
    if (bind_rc != error::OK) {
        std::fprintf(stderr, "[ERROR] demo SetHistoryReader failed: rc=%d\n", bind_rc);
        return false;
    }

    DetectorResult warmup{};
    const SeriesPoint& last_history = demo_points.back();
    const ValueObservation warmup_obs{
        StringRef(demo_options.series_key),
        last_history.bucket_id,
        last_history.value,
        0,
    };
    if (task->SubmitObservation(warmup_obs, &warmup) != error::OK) {
        std::fprintf(stderr, "[ERROR] demo SubmitObservation warmup failed\n");
        return false;
    }

    if (task->RequestRebuild(StringRef(demo_options.series_key),
                             BaselineRebuildReason::kManual) != error::OK) {
        std::fprintf(stderr, "[ERROR] demo RequestRebuild failed\n");
        return false;
    }

    SnapshotState snapshot;
    std::string snapshot_json;
    if (!WaitForFormalReady(task, demo_options.series_key, &snapshot, &snapshot_json)) {
        std::fprintf(stderr, "[ERROR] demo formal model was not ready in time\n");
        return false;
    }

    DetectorResult spike{};
    const ValueObservation spike_obs{
        StringRef(demo_options.series_key),
        last_history.bucket_id + 1,
        last_history.value + 120.0,
        0,
    };
    if (task->SubmitObservation(spike_obs, &spike) != error::OK) {
        std::fprintf(stderr, "[ERROR] demo SubmitObservation spike failed\n");
        return false;
    }

    std::printf("[DEMO] task_config=%s\n", task_config.c_str());
    std::printf("[DEMO] history_fetch_count=%zu\n", reader.fetch_count());
    std::printf("[DEMO] bootstrap_formal_version=%llu, model_state=%s\n",
                static_cast<unsigned long long>(snapshot.formal_model_version),
                snapshot.model_state.c_str());
    std::printf("[DEMO] spike_result score=%.6f confidence=%.6f provider=%s reason=%s direction=%s flags=%s\n",
                spike.normalized_score,
                spike.confidence,
                ProviderName(spike.provider),
                ReasonName(spike.reason),
                DirectionName(spike.direction),
                FlagsToString(spike.flags).c_str());
    std::printf("[DEMO] snapshot=%s\n", snapshot_json.c_str());
    return true;
}

}  // namespace
}  // namespace baseline_test
}  // namespace flowsql

int main(int argc, char** argv) {
    flowsql::baseline_test::CliOptions options;
    std::string err;
    if (!flowsql::baseline_test::ParseArgs(argc, argv, &options, &err)) {
        std::fprintf(stderr, "[ERROR] %s\n", err.c_str());
        flowsql::baseline_test::PrintUsage();
        return 1;
    }

    const bool ok = options.mode == "demo"
                        ? flowsql::baseline_test::RunDemo(options)
                        : flowsql::baseline_test::RunReplay(options);
    return ok ? 0 : 1;
}
