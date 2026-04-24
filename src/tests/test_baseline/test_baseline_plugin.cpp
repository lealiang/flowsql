/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <cassert>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <cmath>
#include <rapidjson/document.h>
#include <string>
#include <thread>
#include <vector>

#include <common/loader.hpp>
#include <common/error_code.h>
#include <framework/interfaces/ibaseline_service.h>
#include <plugins/baseline/common/result_builder.h>
#include <plugins/baseline/model/event_calendar_spec.h>
#include <plugins/baseline/model/formal_predictor.h>
#include <plugins/baseline/model/series_store.h>

using namespace flowsql;

namespace {

struct LoadedBaselineService {
    PluginLoader* loader = nullptr;
    IBaselineService* service = nullptr;

    ~LoadedBaselineService() {
        if (!loader) return;
        loader->StopAll();
        loader->Unload();
    }
};

LoadedBaselineService LoadBaselineService() {
    LoadedBaselineService env;
    env.loader = PluginLoader::Single();

    std::string plugin_dir = get_absolute_process_path();
    std::string plugin_name = "libflowsql_baseline.so";
    const char* relapath[] = {plugin_name.c_str()};
    const char* options[] = {nullptr};

    const int ret = env.loader->Load(plugin_dir.c_str(), relapath, options, 1);
    assert(ret == 0);
    assert(env.loader->StartAll() == 0);

    env.service = static_cast<IBaselineService*>(env.loader->First(IID_BASELINE_SERVICE));
    assert(env.service != nullptr);
    return env;
}

struct ListedTask {
    std::string id;
    std::string name;
    BaselineTaskKind kind = BaselineTaskKind::kValue;
};

std::vector<ListedTask> ListTasks(IBaselineService* service) {
    std::vector<ListedTask> tasks;
    service->ListTasks([&tasks](const char* task_id,
                                const char* task_name,
                                BaselineTaskKind kind) {
        ListedTask item;
        item.id = task_id ? task_id : "";
        item.name = task_name ? task_name : "";
        item.kind = kind;
        tasks.push_back(item);
    });
    return tasks;
}

bool ContainsTask(const std::vector<ListedTask>& tasks,
                  const char* task_id,
                  BaselineTaskKind expected_kind) {
    for (const auto& task : tasks) {
        if (task.id == (task_id ? task_id : "") && task.kind == expected_kind) {
            return true;
        }
    }
    return false;
}

class StaticBaselineSourceResolver : public IBaselineSourceResolver {
 public:
    int ResolveBaselineSource(const BaselineStringRef& key,
                              const BaselineStringRef& feature,
                              std::string* out_config_json) override {
        last_key_ = key.data ? std::string(key.data, key.size) : "";
        last_feature_ = feature.data ? std::string(feature.data, feature.size) : "";
        if (out_config_json) *out_config_json = "";
        return error::OK;
    }

    const std::string& last_key() const { return last_key_; }
    const std::string& last_feature() const { return last_feature_; }

 private:
    std::string last_key_;
    std::string last_feature_;
};

class RoutedBaselineSourceResolver : public IBaselineSourceResolver {
 public:
    int ResolveBaselineSource(const BaselineStringRef& key,
                              const BaselineStringRef& feature,
                              std::string* out_config_json) override {
        last_key_ = key.data ? std::string(key.data, key.size) : "";
        last_feature_ = feature.data ? std::string(feature.data, feature.size) : "";
        ++call_count_;
        if (out_config_json) {
            *out_config_json =
                R"({"baseline_sources":[{"source_key":"svc-source"}]})";
        }
        return error::OK;
    }

    int call_count() const { return call_count_; }
    const std::string& last_key() const { return last_key_; }
    const std::string& last_feature() const { return last_feature_; }

 private:
    int call_count_ = 0;
    std::string last_key_;
    std::string last_feature_;
};

template <typename Predicate>
bool WaitUntil(Predicate&& pred, int timeout_ms = 2000) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

class CountingValueHistoryReader : public IBaselineValueHistoryReader {
 public:
    int Fetch(const HistoryFetchRequest& req,
              std::function<int(const ValueObservation&)> on_point) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_key_ = req.key.data ? std::string(req.key.data, req.key.size) : "";
            last_start_ = req.bucket_start;
            last_end_ = req.bucket_end;
        }
        ++call_count_;
        if (on_point) {
            const int64_t first_bucket =
                (req.bucket_end >= 2) ? (req.bucket_end - 2) : req.bucket_end;
            for (int64_t bucket = first_bucket; bucket <= req.bucket_end; ++bucket) {
                const ValueObservation replay{
                    BaselineStringRef{last_key_.c_str(), static_cast<uint32_t>(last_key_.size())},
                    bucket,
                    static_cast<double>(bucket - first_bucket + 1),
                    0};
                const int rc = on_point(replay);
                if (rc != error::OK) return rc;
            }
            return error::OK;
        }
        return error::OK;
    }

    int call_count() const { return call_count_.load(); }
    std::string last_key() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_key_;
    }
    int64_t last_start() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_start_;
    }
    int64_t last_end() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_end_;
    }

 private:
    mutable std::mutex mutex_;
    std::atomic<int> call_count_{0};
    std::string last_key_;
    int64_t last_start_ = 0;
    int64_t last_end_ = 0;
};

class SinglePointValueHistoryReader : public IBaselineValueHistoryReader {
 public:
    int Fetch(const HistoryFetchRequest& req,
              std::function<int(const ValueObservation&)> on_point) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_key_ = req.key.data ? std::string(req.key.data, req.key.size) : "";
            last_start_ = req.bucket_start;
            last_end_ = req.bucket_end;
        }
        ++call_count_;
        if (!on_point) return error::OK;

        const ValueObservation replay{
            BaselineStringRef{last_key_.c_str(), static_cast<uint32_t>(last_key_.size())},
            req.bucket_end,
            7.0,
            0};
        return on_point(replay);
    }

    int call_count() const { return call_count_.load(); }

 private:
    mutable std::mutex mutex_;
    std::atomic<int> call_count_{0};
    std::string last_key_;
    int64_t last_start_ = 0;
    int64_t last_end_ = 0;
};

class FailingRatioHistoryReader : public IBaselineRatioHistoryReader {
 public:
    int Fetch(const HistoryFetchRequest& req,
              std::function<int(const RatioObservation&)>) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_key_ = req.key.data ? std::string(req.key.data, req.key.size) : "";
            last_start_ = req.bucket_start;
            last_end_ = req.bucket_end;
        }
        ++call_count_;
        return error::UNAVAILABLE;
    }

    int call_count() const { return call_count_.load(); }
    std::string last_key() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_key_;
    }
    int64_t last_start() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_start_;
    }
    int64_t last_end() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_end_;
    }

 private:
    mutable std::mutex mutex_;
    std::atomic<int> call_count_{0};
    std::string last_key_;
    int64_t last_start_ = 0;
    int64_t last_end_ = 0;
};

class CountingRatioHistoryReader : public IBaselineRatioHistoryReader {
 public:
    int Fetch(const HistoryFetchRequest& req,
              std::function<int(const RatioObservation&)> on_point) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_key_ = req.key.data ? std::string(req.key.data, req.key.size) : "";
            last_start_ = req.bucket_start;
            last_end_ = req.bucket_end;
        }
        ++call_count_;
        if (on_point) {
            const int64_t first_bucket =
                (req.bucket_end >= 2) ? (req.bucket_end - 2) : req.bucket_end;
            for (int64_t bucket = first_bucket; bucket <= req.bucket_end; ++bucket) {
                const double numerator =
                    30.0 + 10.0 * static_cast<double>(bucket - first_bucket + 1);
                const double denominator = numerator + 20.0;
                const RatioObservation replay{
                    BaselineStringRef{last_key_.c_str(), static_cast<uint32_t>(last_key_.size())},
                    bucket,
                    numerator,
                    denominator};
                const int rc = on_point(replay);
                if (rc != error::OK) return rc;
            }
        }
        return error::OK;
    }

    int call_count() const { return call_count_.load(); }

 private:
    mutable std::mutex mutex_;
    std::atomic<int> call_count_{0};
    std::string last_key_;
    int64_t last_start_ = 0;
    int64_t last_end_ = 0;
};

class SwitchingValueHistoryReader : public IBaselineValueHistoryReader {
 public:
    int Fetch(const HistoryFetchRequest& req,
              std::function<int(const ValueObservation&)> on_point) override {
        const int call_index = ++call_count_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_key_ = req.key.data ? std::string(req.key.data, req.key.size) : "";
            last_start_ = req.bucket_start;
            last_end_ = req.bucket_end;
            if (call_index == 2) second_fetch_started_ = true;
        }
        cv_.notify_all();

        if (call_index == 2) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return allow_second_fetch_; });
        }

        const double value = call_index == 1 ? 1.0 : 64.0;
        const int64_t replay_span = call_index == 1 ? 2 : 39;
        const int64_t first_bucket =
            std::max<int64_t>(req.bucket_start, req.bucket_end - replay_span);
        for (int64_t bucket = first_bucket; bucket <= req.bucket_end; ++bucket) {
            const ValueObservation replay{
                BaselineStringRef{last_key_.c_str(), static_cast<uint32_t>(last_key_.size())},
                bucket,
                value,
                0};
            const int rc = on_point ? on_point(replay) : error::OK;
            if (rc != error::OK) return rc;
        }
        return error::OK;
    }

    int call_count() const { return call_count_.load(); }

    bool second_fetch_started() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return second_fetch_started_;
    }

    void AllowSecondFetch() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            allow_second_fetch_ = true;
        }
        cv_.notify_all();
    }

 private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<int> call_count_{0};
    std::string last_key_;
    int64_t last_start_ = 0;
    int64_t last_end_ = 0;
    bool second_fetch_started_ = false;
    bool allow_second_fetch_ = false;
};

class SwitchingRatioHistoryReader : public IBaselineRatioHistoryReader {
 public:
    int Fetch(const HistoryFetchRequest& req,
              std::function<int(const RatioObservation&)> on_point) override {
        const int call_index = ++call_count_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_key_ = req.key.data ? std::string(req.key.data, req.key.size) : "";
            last_start_ = req.bucket_start;
            last_end_ = req.bucket_end;
            if (call_index == 2) second_fetch_started_ = true;
        }
        cv_.notify_all();

        if (call_index == 2) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return allow_second_fetch_; });
        }

        const double numerator = call_index == 1 ? 10.0 : 23.0;
        const double denominator = 100.0;
        const int64_t replay_span = call_index == 1 ? 2 : 39;
        const int64_t first_bucket =
            std::max<int64_t>(req.bucket_start, req.bucket_end - replay_span);
        for (int64_t bucket = first_bucket; bucket <= req.bucket_end; ++bucket) {
            const RatioObservation replay{
                BaselineStringRef{last_key_.c_str(), static_cast<uint32_t>(last_key_.size())},
                bucket,
                numerator,
                denominator};
            const int rc = on_point ? on_point(replay) : error::OK;
            if (rc != error::OK) return rc;
        }
        return error::OK;
    }

    int call_count() const { return call_count_.load(); }

    bool second_fetch_started() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return second_fetch_started_;
    }

    void AllowSecondFetch() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            allow_second_fetch_ = true;
        }
        cv_.notify_all();
    }

 private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<int> call_count_{0};
    std::string last_key_;
    int64_t last_start_ = 0;
    int64_t last_end_ = 0;
    bool second_fetch_started_ = false;
    bool allow_second_fetch_ = false;
};

struct OwnedRelationMetricReplay {
    double total = 0.0;
    uint32_t active_count = 0;
    std::vector<double> values;
    uint32_t flags = kRelationMetricHasActiveCount;
};

struct OwnedRelationReplayBlock {
    int64_t bucket_id = 0;
    std::vector<uint32_t> group_idx;
    std::vector<OwnedRelationMetricReplay> metrics;
};

class ScriptedRelationHistoryReader : public IBaselineRelationHistoryReader {
 public:
    void AddFetchScript(std::vector<OwnedRelationReplayBlock> blocks) {
        scripts_.push_back(std::move(blocks));
    }

    int Fetch(const HistoryFetchRequest& req,
              std::function<int(const RelationObservationBlock&)> on_block) override {
        const std::string key = req.key.data ? std::string(req.key.data, req.key.size) : "";
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_key_ = key;
            last_start_ = req.bucket_start;
            last_end_ = req.bucket_end;
        }

        const int call_index = call_count_.fetch_add(1);
        const size_t script_index =
            std::min(static_cast<size_t>(call_index), scripts_.empty() ? size_t{0}
                                                                       : scripts_.size() - 1);
        if (scripts_.empty()) return error::OK;

        for (const auto& owned_block : scripts_[script_index]) {
            metric_views_.clear();
            metric_views_.reserve(owned_block.metrics.size());
            for (const auto& metric : owned_block.metrics) {
                metric_views_.push_back(RelationMetricBlock{
                    metric.total,
                    metric.flags,
                    metric.active_count,
                    metric.values.empty() ? nullptr : metric.values.data()});
            }

            const RelationObservationBlock view{
                BaselineStringRef{key.c_str(), static_cast<uint32_t>(key.size())},
                owned_block.bucket_id,
                static_cast<uint32_t>(owned_block.group_idx.size()),
                owned_block.group_idx.empty() ? nullptr : owned_block.group_idx.data(),
                static_cast<uint32_t>(metric_views_.size()),
                metric_views_.empty() ? nullptr : metric_views_.data(),
            };
            const int rc = on_block ? on_block(view) : error::OK;
            if (rc != error::OK) return rc;
        }
        return error::OK;
    }

    int call_count() const { return call_count_.load(); }

 private:
    mutable std::mutex mutex_;
    std::atomic<int> call_count_{0};
    std::string last_key_;
    int64_t last_start_ = 0;
    int64_t last_end_ = 0;
    std::vector<std::vector<OwnedRelationReplayBlock>> scripts_;
    std::vector<RelationMetricBlock> metric_views_;
};

rapidjson::Document ParseJson(const std::string& json) {
    rapidjson::Document doc;
    doc.Parse(json.c_str());
    assert(!doc.HasParseError());
    assert(doc.IsObject());
    return doc;
}

BaselineStringRef StringRef(const std::string& value) {
    return BaselineStringRef{value.c_str(), static_cast<uint32_t>(value.size())};
}

void AssertDoubleNear(double actual, double expected, double epsilon = 1e-9) {
    assert(std::fabs(actual - expected) <= epsilon);
}

double Median(std::vector<double> values) {
    assert(!values.empty());
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

double ComputeT1SigmaRef(const std::vector<double>& x_values,
                         double intercept_x) {
    std::vector<double> residuals;
    residuals.reserve(x_values.size());
    for (double x : x_values) {
        residuals.push_back(x - intercept_x);
    }

    const double median_r = Median(residuals);
    std::vector<double> abs_deviation;
    abs_deviation.reserve(residuals.size());
    for (double r : residuals) {
        abs_deviation.push_back(std::fabs(r - median_r));
    }

    return std::max(1e-3, 1.4826 * Median(abs_deviation));
}

double ComputeT2RateCorePredictValue(const std::vector<double>& numerators,
                                     const std::vector<double>& denominators) {
    assert(numerators.size() == denominators.size());
    assert(!numerators.empty());

    double numerator_sum = 0.0;
    double denominator_sum = 0.0;
    for (std::size_t i = 0; i < numerators.size(); ++i) {
        numerator_sum += numerators[i];
        denominator_sum += denominators[i];
    }

    const double m0 = numerator_sum / denominator_sum;
    const double alpha0 = 2.0 * m0;
    const double beta0 = 2.0 * (1.0 - m0);

    double weighted_eta = 0.0;
    double total_weight = 0.0;
    for (std::size_t i = 0; i < numerators.size(); ++i) {
        const double smoothed =
            (numerators[i] + alpha0) / (denominators[i] + alpha0 + beta0);
        const double eta = std::log(smoothed / (1.0 - smoothed));
        weighted_eta += eta * denominators[i];
        total_weight += denominators[i];
    }

    const double eta_hat = weighted_eta / total_weight;
    return 1.0 / (1.0 + std::exp(-eta_hat));
}

}  // namespace

static void TestBaselineServiceHeaderAndIid() {
    std::printf("[TEST] Baseline service IID/header contract...\n");
    const Guid iid = IID_BASELINE_SERVICE;
    (void)iid;
    std::printf("[PASS] Baseline service IID/header contract\n");
}

static void TestBaselinePluginLoadAndQuery() {
    std::printf("[TEST] Baseline plugin load and query...\n");
    auto env = LoadBaselineService();
    assert(env.service != nullptr);

    std::printf("[PASS] Baseline plugin load and query\n");
}

static void TestBaselineTaskLifecycleAndConfigValidation() {
    std::printf("[TEST] Baseline task lifecycle and config validation...\n");

    auto env = LoadBaselineService();
    auto* service = env.service;

    const char* value_cfg =
        R"({"name":"avg_rtt","key":"service","feature":"avg_rtt","feature_type":"t1b","feature_profile":"cont_core","delta":60,"tz":"Asia/Shanghai"})";
    const char* ratio_cfg =
        R"({"name":"success_rate","key":"service","feature":"success_rate","feature_type":"t2","feature_profile":"rate_core","delta":60,"tz":"Asia/Shanghai"})";
    const char* relation_cfg =
        R"({"name":"client_group_mix","feature_base":"client_group_mix","group_space_id":"client_group","delta":60,"tz":"Asia/Shanghai","metric_set_id":"net_metrics","metrics":["conn_count","bps"],"encode_type":"exact_sparse","support_policy":{"k_support":16,"min_hist_share":0.005,"min_active_ratio":0.2},"summary_policy":{"k_head":5,"k_stable":5},"event_calendar_spec":{"calendar_id":"relation-calendar","calendar_version":"v1","entries":[{"event_code":"month_close","scope_type":"global","alignment_mode":"local_wall_clock","start_ts":1711900800,"end_ts":1711987199,"enabled":true,"tz":"Asia/Shanghai"}]}})";

    IBaselineValueTask* value_task = nullptr;
    IBaselineRatioTask* ratio_task = nullptr;
    IBaselineRelationTask* relation_task = nullptr;
    StaticBaselineSourceResolver relation_resolver;

    assert(service->CreateValueTask(value_cfg, &value_task) == error::OK);
    assert(value_task != nullptr);
    const std::string value_task_id = value_task->Id();
    assert(std::string(value_task->Name()) == "avg_rtt");
    assert(value_task->Kind() == BaselineTaskKind::kValue);
    assert(std::string(value_task->ConfigJson()) == value_cfg);

    assert(service->CreateRatioTask(ratio_cfg, &ratio_task) == error::OK);
    assert(ratio_task != nullptr);
    const std::string ratio_task_id = ratio_task->Id();
    assert(std::string(ratio_task->Name()) == "success_rate");
    assert(ratio_task->Kind() == BaselineTaskKind::kRatio);
    assert(std::string(ratio_task->ConfigJson()) == ratio_cfg);

    assert(service->CreateRelationTask(relation_cfg, &relation_resolver, &relation_task) ==
           error::OK);
    assert(relation_task != nullptr);
    const std::string relation_task_id = relation_task->Id();
    assert(std::string(relation_task->Name()) == "client_group_mix");
    assert(relation_task->Kind() == BaselineTaskKind::kRelation);
    assert(std::string(relation_task->ConfigJson()) == relation_cfg);

    const auto listed_before_close = ListTasks(service);
    assert(ContainsTask(listed_before_close, value_task_id.c_str(), BaselineTaskKind::kValue));
    assert(ContainsTask(listed_before_close, ratio_task_id.c_str(), BaselineTaskKind::kRatio));
    assert(ContainsTask(listed_before_close, relation_task_id.c_str(), BaselineTaskKind::kRelation));

    std::string task_snapshot;
    assert(value_task->QueryTaskSnapshotJson(&task_snapshot) == error::OK);
    assert(task_snapshot.find("\"name\":\"avg_rtt\"") != std::string::npos);
    assert(relation_task->QueryTaskSnapshotJson(&task_snapshot) == error::OK);
    auto relation_task_doc = ParseJson(task_snapshot);
    assert(relation_task_doc["delta"].GetInt64() == 60);
    assert(std::string(relation_task_doc["tz"].GetString()) == "Asia/Shanghai");
    assert(relation_task_doc["event_calendar_present"].GetBool() == true);
    assert(std::string(relation_task_doc["event_calendar_id"].GetString()) ==
           "relation-calendar");
    assert(std::string(relation_task_doc["event_calendar_version"].GetString()) == "v1");
    assert(relation_task_doc["event_calendar_entry_count"].GetUint64() == 1);
    assert(relation_task_doc["source_resolver_bound"].GetBool() == true);

    assert(value_task->Close() == error::OK);
    assert(ratio_task->Close() == error::OK);
    assert(relation_task->Close() == error::OK);

    const auto listed_after_close = ListTasks(service);
    assert(!ContainsTask(listed_after_close, value_task_id.c_str(), BaselineTaskKind::kValue));
    assert(!ContainsTask(listed_after_close, ratio_task_id.c_str(), BaselineTaskKind::kRatio));
    assert(!ContainsTask(listed_after_close, relation_task_id.c_str(), BaselineTaskKind::kRelation));

    IBaselineValueTask* bad_value_task = reinterpret_cast<IBaselineValueTask*>(0x1);
    IBaselineRatioTask* bad_ratio_task = reinterpret_cast<IBaselineRatioTask*>(0x1);
    IBaselineRelationTask* bad_relation_task = reinterpret_cast<IBaselineRelationTask*>(0x1);

    assert(service->CreateValueTask(R"({"name":"bad","feature":"avg_rtt"})", &bad_value_task) == error::BAD_REQUEST);
    assert(bad_value_task == nullptr);

    assert(service->CreateRatioTask(R"({"name":"bad","feature":"success_rate","delta":60})", &bad_ratio_task) == error::BAD_REQUEST);
    assert(bad_ratio_task == nullptr);

    assert(service->CreateRelationTask(R"({"name":"bad","feature_base":"client_group_mix"})", nullptr, &bad_relation_task) == error::BAD_REQUEST);
    assert(bad_relation_task == nullptr);

    const char* bad_source_cfg =
        R"({"name":"avg_rtt","key":"svc-a","feature":"avg_rtt","feature_type":"t1b","feature_profile":"cont_core","delta":60,"tz":"Asia/Shanghai","baseline_source_configs":[{"key":"svc-a","baseline_sources":[{"source_key":"svc-a"}]}]})";
    assert(service->CreateValueTask(bad_source_cfg, &bad_value_task) == error::BAD_REQUEST);
    assert(bad_value_task == nullptr);

    std::printf("[PASS] Baseline task lifecycle and config validation\n");
}

static void TestBaselineSeriesStoreCommonState() {
    std::printf("[TEST] Baseline series store common state...\n");

    baseline::SeriesStore store;
    const BaselineStringRef key{"service-a", 9};

    baseline::SeriesUpdateResult first;
    assert(store.ApplyObservation(key, 100, true, &first) == error::OK);
    assert((first.flags & kBaselineFlagColdStart) != 0);
    assert(first.gap == 0);
    assert(first.persistence == 1);

    DetectorResult result{};
    baseline::FillBaseResult(first, &result);
    assert(result.status == error::OK);
    assert(result.persistence == 1);
    assert((result.flags & kBaselineFlagColdStart) != 0);

    baseline::SeriesUpdateResult gap_update;
    assert(store.ApplyObservation(key, 103, true, &gap_update) == error::OK);
    assert((gap_update.flags & kBaselineFlagGapBefore) != 0);
    assert(gap_update.gap == 2);
    assert(gap_update.persistence == 2);

    baseline::SeriesUpdateResult normal_update;
    assert(store.ApplyObservation(key, 104, false, &normal_update) == error::OK);
    assert(normal_update.gap == 0);
    assert(normal_update.persistence == 0);

    baseline::SeriesUpdateResult out_of_order;
    assert(store.ApplyObservation(key, 102, true, &out_of_order) == error::BAD_REQUEST);
    assert((out_of_order.flags & kBaselineFlagOutOfOrder) != 0);
    assert(out_of_order.persistence == 0);

    baseline::SeriesState state;
    assert(store.GetState(key, &state) == error::OK);
    assert(state.initialized);
    assert(state.last_bucket_id == 104);
    assert(state.observation_count == 3);

    std::printf("[PASS] Baseline series store common state\n");
}

static void TestBaselineValueTaskHotPath() {
    std::printf("[TEST] Baseline value task hot path...\n");

    auto env = LoadBaselineService();
    auto* service = env.service;

    const char* t1a_cfg =
        R"({"name":"bytes_total","key":"service","feature":"bytes_total","feature_type":"t1a","feature_profile":"traffic","delta":60,"tz":"Asia/Shanghai"})";
    const char* t1b_cfg =
        R"({"name":"avg_rtt","key":"service","feature":"avg_rtt","feature_type":"t1b","feature_profile":"cont_core","delta":60,"tz":"Asia/Shanghai"})";

    IBaselineValueTask* t1a_task = nullptr;
    IBaselineValueTask* t1b_task = nullptr;
    assert(service->CreateValueTask(t1a_cfg, &t1a_task) == error::OK);
    assert(service->CreateValueTask(t1b_cfg, &t1b_task) == error::OK);
    assert(t1a_task != nullptr);
    assert(t1b_task != nullptr);

    DetectorResult t1a_result{};
    const ValueObservation t1a_obs{BaselineStringRef{"svc-a", 5}, 100, 2048.0, 0};
    assert(t1a_task->SubmitObservation(t1a_obs, &t1a_result) == error::OK);
    assert(t1a_result.status == error::OK);
    assert((t1a_result.flags & kBaselineFlagColdStart) != 0);
    assert(t1a_result.raw_score == 0.0);
    assert(t1a_result.normalized_score == 0.0);

    std::string t1a_series_snapshot;
    assert(t1a_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-a", 5}, &t1a_series_snapshot) == error::OK);
    assert(t1a_series_snapshot.find("\"last_bucket_id\":100") != std::string::npos);
    assert(t1a_series_snapshot.find("\"observation_count\":1") != std::string::npos);

    DetectorResult low_sample_result{};
    const ValueObservation low_sample_obs{BaselineStringRef{"svc-b", 5}, 200, 12.5, 10};
    assert(t1b_task->SubmitObservation(low_sample_obs, &low_sample_result) == error::OK);
    assert(low_sample_result.status == error::OK);
    assert(low_sample_result.raw_score == 0.0);
    assert(low_sample_result.normalized_score == 0.0);

    std::string low_sample_snapshot;
    assert(t1b_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-b", 5}, &low_sample_snapshot) == error::OK);
    assert(low_sample_snapshot.find("\"last_gate_score\":false") != std::string::npos);
    assert(low_sample_snapshot.find("\"last_gate_shift\":false") != std::string::npos);
    assert(low_sample_snapshot.find("\"last_sample_count\":10") != std::string::npos);

    DetectorResult enough_sample_result{};
    const ValueObservation enough_sample_obs{BaselineStringRef{"svc-b", 5}, 201, 13.5, 50};
    assert(t1b_task->SubmitObservation(enough_sample_obs, &enough_sample_result) == error::OK);
    assert(enough_sample_result.status == error::OK);

    std::string enough_sample_snapshot;
    assert(t1b_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-b", 5}, &enough_sample_snapshot) == error::OK);
    assert(enough_sample_snapshot.find("\"last_gate_score\":true") != std::string::npos);
    assert(enough_sample_snapshot.find("\"last_gate_shift\":false") != std::string::npos);
    assert(enough_sample_snapshot.find("\"last_sample_count\":50") != std::string::npos);

    DetectorResult out_of_order_result{};
    const ValueObservation out_of_order_obs{BaselineStringRef{"svc-b", 5}, 199, 11.5, 50};
    assert(t1b_task->SubmitObservation(out_of_order_obs, &out_of_order_result) == error::BAD_REQUEST);
    assert(out_of_order_result.status == error::BAD_REQUEST);
    assert((out_of_order_result.flags & kBaselineFlagOutOfOrder) != 0);

    assert(t1a_task->Close() == error::OK);
    assert(t1b_task->Close() == error::OK);

    std::printf("[PASS] Baseline value task hot path\n");
}

static void TestBaselineRatioTaskHotPath() {
    std::printf("[TEST] Baseline ratio task hot path...\n");

    auto env = LoadBaselineService();
    auto* service = env.service;

    const char* ratio_cfg =
        R"({"name":"success_rate","key":"service","feature":"success_rate","feature_type":"t2","feature_profile":"rate_core","delta":60,"tz":"Asia/Shanghai"})";

    IBaselineRatioTask* ratio_task = nullptr;
    assert(service->CreateRatioTask(ratio_cfg, &ratio_task) == error::OK);
    assert(ratio_task != nullptr);

    DetectorResult low_den_result{};
    const RatioObservation low_den_obs{BaselineStringRef{"svc-r", 5}, 300, 18.0, 20.0};
    assert(ratio_task->SubmitObservation(low_den_obs, &low_den_result) == error::OK);
    assert(low_den_result.status == error::OK);
    assert(low_den_result.raw_score == 0.0);
    assert(low_den_result.normalized_score == 0.0);

    std::string low_den_snapshot;
    assert(ratio_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-r", 5}, &low_den_snapshot) == error::OK);
    assert(low_den_snapshot.find("\"last_gate_score\":false") != std::string::npos);
    assert(low_den_snapshot.find("\"last_gate_shift\":false") != std::string::npos);
    assert(low_den_snapshot.find("\"last_denominator\":20.0") != std::string::npos);

    DetectorResult enough_den_result{};
    const RatioObservation enough_den_obs{BaselineStringRef{"svc-r", 5}, 301, 55.0, 60.0};
    assert(ratio_task->SubmitObservation(enough_den_obs, &enough_den_result) == error::OK);
    assert(enough_den_result.status == error::OK);

    std::string enough_den_snapshot;
    assert(ratio_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-r", 5}, &enough_den_snapshot) == error::OK);
    assert(enough_den_snapshot.find("\"last_gate_score\":true") != std::string::npos);
    assert(enough_den_snapshot.find("\"last_gate_shift\":false") != std::string::npos);
    assert(enough_den_snapshot.find("\"last_denominator\":60.0") != std::string::npos);

    DetectorResult out_of_order_result{};
    const RatioObservation out_of_order_obs{BaselineStringRef{"svc-r", 5}, 299, 10.0, 20.0};
    assert(ratio_task->SubmitObservation(out_of_order_obs, &out_of_order_result) == error::BAD_REQUEST);
    assert(out_of_order_result.status == error::BAD_REQUEST);
    assert((out_of_order_result.flags & kBaselineFlagOutOfOrder) != 0);

    assert(ratio_task->Close() == error::OK);

    std::printf("[PASS] Baseline ratio task hot path\n");
}

static void TestBaselineRelationTaskHotPath() {
    std::printf("[TEST] Baseline relation task hot path...\n");

    auto env = LoadBaselineService();
    auto* service = env.service;

    const char* relation_cfg =
        R"({"name":"client_group_mix","feature_base":"client_group_mix","group_space_id":"client_group","group_space_version":"v1","delta":60,"tz":"Asia/Shanghai","metric_set_id":"net_metrics","metrics":["conn_count","bps"],"encode_type":"exact_sparse","support_policy":{"k_support":8,"min_hist_share":0.005,"min_active_ratio":0.2},"summary_policy":{"k_head":2,"k_stable":2},"event_calendar_spec":{"calendar_id":"relation-calendar","calendar_version":"v1","entries":[{"event_code":"global_event","scope_type":"global","alignment_mode":"local_wall_clock","start_ts":1711900800,"end_ts":1711987199,"enabled":true,"tz":"Asia/Shanghai"},{"event_code":"entropy_event","scope_type":"feature","alignment_mode":"local_wall_clock","start_ts":1711900800,"end_ts":1711987199,"enabled":true,"feature":"client_group_mix_conn_count_entropy_shannon","tz":"Asia/Shanghai"}]}})";

    IBaselineRelationTask* relation_task = nullptr;
    RoutedBaselineSourceResolver resolver;
    assert(service->CreateRelationTask(relation_cfg, &resolver, &relation_task) == error::OK);
    assert(relation_task != nullptr);

    const uint32_t group_idx[] = {11, 12, 50};
    const double conn_values[] = {50.0, 30.0, 20.0};
    const double bps_values[] = {400.0, 350.0, 250.0};
    const RelationMetricBlock metrics[] = {
        {100.0, kRelationMetricHasActiveCount, 3, conn_values},
        {1000.0, kRelationMetricHasActiveCount, 3, bps_values},
    };
    const RelationObservationBlock block{
        BaselineStringRef{"svc-relation", 12},
        10,
        3,
        group_idx,
        2,
        metrics,
    };

    FusionResult result;
    assert(relation_task->SubmitBlock(block, &result) == error::OK);
    assert(result.ts == 10);

    std::string task_snapshot;
    assert(relation_task->QueryTaskSnapshotJson(&task_snapshot) == error::OK);
    auto task_doc = ParseJson(task_snapshot);
    assert(task_doc["routed_feature_count"].GetUint64() > 0);
    assert(task_doc["key_runtime_count"].GetUint64() == 1);
    assert(task_doc["event_calendar_present"].GetBool() == true);
    assert(task_doc["source_resolver_bound"].GetBool() == true);

    std::string series_snapshot;
    assert(relation_task->QuerySeriesSnapshotJson(
               BaselineStringRef{"svc-relation", 12}, &series_snapshot) == error::OK);
    auto series_doc = ParseJson(series_snapshot);
    assert(series_doc["seen_block_count"].GetUint64() == 1);
    assert(series_doc["basis_metric_count"].GetUint64() == 2);
    assert(series_doc["last_fusion_result"]["available"].GetBool() == true);
    assert(series_doc["metrics"].IsArray());
    assert(series_doc["metrics"].Size() == 2);
    assert(series_doc["metrics"][0]["service_basis"]["support_explicit"].IsArray());
    assert(series_doc["metrics"][0]["service_basis"]["support_explicit"].Size() >= 1);
    assert(series_doc["metrics"][0]["routed_features"].IsArray());
    assert(series_doc["metrics"][0]["routed_feature_count"].GetUint64() >= 6);
    assert(series_doc["metrics"][0]["routed_features"][0]["delta"].GetInt64() == 60);
    assert(std::string(series_doc["metrics"][0]["routed_features"][0]["tz"].GetString()) ==
           "Asia/Shanghai");
    assert(series_doc["metrics"][0]["routed_features"][0]["baseline_source_present"].GetBool() ==
           true);
    assert(series_doc["metrics"][0]["routed_features"][0]["event_calendar_present"].GetBool() ==
           true);
    assert(series_doc["metrics"][0]["routed_features"][0]["last_detector_result"]["available"]
               .GetBool() == true);
    assert(resolver.call_count() > 0);
    assert(resolver.last_key() == "svc-relation");

    assert(relation_task->Close() == error::OK);

    std::printf("[PASS] Baseline relation task hot path\n");
}

static void TestBaselineRelationTaskRebuildDirectApply() {
    std::printf("[TEST] Baseline relation task rebuild direct apply...\n");

    auto env = LoadBaselineService();
    auto* service = env.service;

    const char* relation_cfg =
        R"({"name":"client_group_mix","feature_base":"client_group_mix","group_space_id":"client_group","group_space_version":"v1","delta":60,"tz":"Asia/Shanghai","metric_set_id":"net_metrics","metrics":["conn_count"],"encode_type":"exact_sparse","support_policy":{"k_support":8,"min_hist_share":0.005,"min_active_ratio":0.2},"summary_policy":{"k_head":2,"k_stable":2}})";

    IBaselineRelationTask* relation_task = nullptr;
    assert(service->CreateRelationTask(relation_cfg, nullptr, &relation_task) == error::OK);
    assert(relation_task != nullptr);

    ScriptedRelationHistoryReader reader;
    reader.AddFetchScript({
        {100, {11, 12, 13}, {{100.0, 3, {78.0, 17.0, 5.0}}}},
        {101, {11, 12, 13}, {{100.0, 3, {76.0, 18.0, 6.0}}}},
        {102, {11, 12, 13}, {{100.0, 3, {80.0, 15.0, 5.0}}}},
        {103, {11, 12, 13}, {{100.0, 3, {79.0, 16.0, 5.0}}}},
        {104, {11, 12, 13}, {{100.0, 3, {77.0, 17.0, 6.0}}}},
        {105, {11, 12, 13}, {{100.0, 3, {78.0, 16.0, 6.0}}}},
    });
    assert(relation_task->SetHistoryReader(&reader) == error::OK);

    assert(relation_task->RequestRebuild(BaselineStringRef{"svc-relation-rebuild", 20},
                                         BaselineRebuildReason::kManual) == error::OK);
    assert(WaitUntil([&reader]() { return reader.call_count() == 1; }));
    assert(WaitUntil([&relation_task]() {
        std::string snapshot;
        if (relation_task->QueryTaskSnapshotJson(&snapshot) != error::OK) return false;
        auto doc = ParseJson(snapshot);
        return doc["rebuild_completed"].GetUint64() == 1;
    }));

    std::string task_snapshot;
    assert(relation_task->QueryTaskSnapshotJson(&task_snapshot) == error::OK);
    auto task_doc = ParseJson(task_snapshot);
    assert(task_doc["rebuild_completed"].GetUint64() == 1);

    std::string series_snapshot;
    assert(relation_task->QuerySeriesSnapshotJson(
               BaselineStringRef{"svc-relation-rebuild", 20}, &series_snapshot) == error::OK);
    auto series_doc = ParseJson(series_snapshot);
    assert(std::string(series_doc["switch_state"].GetString()) == "direct_apply");
    assert(std::string(series_doc["candidate_state"].GetString()) == "none");
    assert(series_doc["basis_metric_count"].GetUint64() == 1);
    assert(series_doc["validation_feature_count"].GetUint64() == 0);
    assert(series_doc["metrics"].IsArray());
    assert(series_doc["metrics"].Size() == 1);
    assert(series_doc["metrics"][0]["basis_version"].GetUint64() == 1);

    assert(relation_task->Close() == error::OK);
    std::printf("[PASS] Baseline relation task rebuild direct apply\n");
}

static void TestBaselineRelationTaskRebuildFormalApply() {
    std::printf("[TEST] Baseline relation task rebuild formal apply...\n");

    auto env = LoadBaselineService();
    auto* service = env.service;

    const char* relation_cfg =
        R"({"name":"client_group_mix","feature_base":"client_group_mix","group_space_id":"client_group","group_space_version":"v1","delta":60,"tz":"Asia/Shanghai","metric_set_id":"net_metrics","metrics":["conn_count"],"encode_type":"exact_sparse","support_policy":{"k_support":8,"min_hist_share":0.005,"min_active_ratio":0.2},"summary_policy":{"k_head":2,"k_stable":2}})";

    IBaselineRelationTask* relation_task = nullptr;
    assert(service->CreateRelationTask(relation_cfg, nullptr, &relation_task) == error::OK);
    assert(relation_task != nullptr);

    ScriptedRelationHistoryReader reader;
    std::vector<OwnedRelationReplayBlock> first_script;
    std::vector<OwnedRelationReplayBlock> second_script;
    for (int i = 0; i < 40; ++i) {
        first_script.push_back(OwnedRelationReplayBlock{
            200 + i,
            {11, 12, 13},
            {{100.0, 3, {82.0 - static_cast<double>(i % 5),
                          13.0 + static_cast<double>(i % 4),
                          5.0}}}});
        second_script.push_back(OwnedRelationReplayBlock{
            300 + i,
            {11, 12, 13},
            {{100.0, 3, {42.0 - static_cast<double>(i % 4),
                          53.0 + static_cast<double>(i % 4),
                          5.0}}}});
    }
    reader.AddFetchScript(std::move(first_script));
    reader.AddFetchScript(std::move(second_script));
    assert(relation_task->SetHistoryReader(&reader) == error::OK);

    const BaselineStringRef key{"svc-relation-formal", 19};
    assert(relation_task->RequestRebuild(key, BaselineRebuildReason::kManual) == error::OK);
    assert(WaitUntil([&reader]() { return reader.call_count() == 1; }));

    assert(relation_task->RequestRebuild(key, BaselineRebuildReason::kManual) == error::OK);
    assert(WaitUntil([&reader]() { return reader.call_count() == 2; }));
    assert(WaitUntil([&relation_task]() {
        std::string snapshot;
        if (relation_task->QueryTaskSnapshotJson(&snapshot) != error::OK) return false;
        auto doc = ParseJson(snapshot);
        return doc["rebuild_completed"].GetUint64() >= 2;
    }));

    std::string series_snapshot;
    assert(relation_task->QuerySeriesSnapshotJson(key, &series_snapshot) == error::OK);
    auto series_doc = ParseJson(series_snapshot);
    assert(std::string(series_doc["switch_state"].GetString()) != "");
    assert(series_doc["last_candidate_loss"].GetDouble() >= 0.0);
    assert(series_doc["last_incumbent_loss"].GetDouble() >= 0.0);
    assert(series_doc["metrics"].IsArray());
    assert(series_doc["metrics"].Size() == 1);
    assert(series_doc["metrics"][0]["basis_version"].GetUint64() >= 1);

    assert(relation_task->Close() == error::OK);
    std::printf("[PASS] Baseline relation task rebuild formal apply\n");
}

static void TestBaselineRebuildInfrastructure() {
    std::printf("[TEST] Baseline rebuild infrastructure...\n");

    auto env = LoadBaselineService();
    auto* service = env.service;

    const char* value_cfg =
        R"({"name":"bytes_total","key":"service","feature":"bytes_total","feature_type":"t1a","feature_profile":"traffic","delta":60,"tz":"Asia/Shanghai"})";
    const char* ratio_cfg =
        R"({"name":"success_rate","key":"service","feature":"success_rate","feature_type":"t2","feature_profile":"rate_core","delta":60,"tz":"Asia/Shanghai"})";

    IBaselineValueTask* value_task = nullptr;
    IBaselineRatioTask* ratio_task = nullptr;
    assert(service->CreateValueTask(value_cfg, &value_task) == error::OK);
    assert(service->CreateRatioTask(ratio_cfg, &ratio_task) == error::OK);
    assert(value_task != nullptr);
    assert(ratio_task != nullptr);

    CountingValueHistoryReader value_reader;
    FailingRatioHistoryReader ratio_reader;
    assert(value_task->SetHistoryReader(&value_reader) == error::OK);
    assert(ratio_task->SetHistoryReader(&ratio_reader) == error::OK);

    DetectorResult value_result{};
    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-rebuild", 11}, 400, 128.0, 0},
               &value_result) == error::OK);
    DetectorResult ratio_result{};
    assert(ratio_task->SubmitObservation(
               RatioObservation{BaselineStringRef{"svc-rebuild", 11}, 500, 45.0, 60.0},
               &ratio_result) == error::OK);

    assert(value_task->RequestRebuild(BaselineStringRef{"svc-rebuild", 11},
                                      BaselineRebuildReason::kManual) == error::OK);
    assert(ratio_task->RequestRebuild(BaselineStringRef{"svc-rebuild", 11},
                                      BaselineRebuildReason::kManual) == error::OK);

    assert(WaitUntil([&value_reader]() { return value_reader.call_count() == 1; }));
    assert(WaitUntil([&ratio_reader]() { return ratio_reader.call_count() == 1; }));
    assert(WaitUntil([&value_task]() {
        std::string snapshot;
        if (value_task->QueryTaskSnapshotJson(&snapshot) != error::OK) return false;
        return snapshot.find("\"rebuild_completed\":1") != std::string::npos;
    }));
    assert(WaitUntil([&ratio_task]() {
        std::string snapshot;
        if (ratio_task->QueryTaskSnapshotJson(&snapshot) != error::OK) return false;
        return snapshot.find("\"rebuild_completed\":1") != std::string::npos;
    }));

    assert(value_reader.last_key() == "svc-rebuild");
    assert(value_reader.last_start() == 0);
    assert(value_reader.last_end() == 400);

    assert(ratio_reader.last_key() == "svc-rebuild");
    assert(ratio_reader.last_start() == 0);
    assert(ratio_reader.last_end() == 500);

    std::string value_task_snapshot;
    assert(value_task->QueryTaskSnapshotJson(&value_task_snapshot) == error::OK);
    assert(value_task_snapshot.find("\"reader_bound\":true") != std::string::npos);
    assert(value_task_snapshot.find("\"last_rebuild_status\":0") != std::string::npos);
    assert(value_task_snapshot.find("\"last_rebuild_reason\":\"manual\"") != std::string::npos);
    assert(value_task_snapshot.find("\"last_rebuild_key\":\"svc-rebuild\"") != std::string::npos);

    std::string value_series_snapshot;
    assert(value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-rebuild", 11},
                                               &value_series_snapshot) == error::OK);
    assert(value_series_snapshot.find("\"formal_ready\":true") != std::string::npos);
    assert(value_series_snapshot.find("\"formal_model_version\":1") != std::string::npos);
    assert(value_series_snapshot.find("\"formal_model_kind\":\"value_baseline\"") != std::string::npos);
    assert(value_series_snapshot.find("\"candidate_generation\":1") != std::string::npos);
    assert(value_series_snapshot.find("\"candidate_state\":\"none\"") != std::string::npos);
    assert(value_series_snapshot.find("\"candidate_model_kind\":\"none\"") != std::string::npos);
    assert(value_series_snapshot.find("\"switch_state\":\"direct_apply\"") != std::string::npos);
    assert(value_series_snapshot.find("\"last_rebuild_bucket_start\":0") != std::string::npos);
    assert(value_series_snapshot.find("\"last_rebuild_bucket_end\":400") != std::string::npos);
    assert(value_series_snapshot.find("\"last_replay_observation_count\":3") != std::string::npos);
    assert(value_series_snapshot.find("\"last_replay_first_bucket_id\":398") != std::string::npos);
    assert(value_series_snapshot.find("\"last_replay_last_bucket_id\":400") != std::string::npos);
    assert(value_series_snapshot.find("\"last_train_observation_count\":3") != std::string::npos);
    assert(value_series_snapshot.find("\"last_train_first_bucket_id\":398") != std::string::npos);
    assert(value_series_snapshot.find("\"last_train_last_bucket_id\":400") != std::string::npos);
    assert(value_series_snapshot.find("\"last_holdout_observation_count\":0") != std::string::npos);

    std::string ratio_task_snapshot;
    assert(ratio_task->QueryTaskSnapshotJson(&ratio_task_snapshot) == error::OK);
    assert(ratio_task_snapshot.find("\"reader_bound\":true") != std::string::npos);
    assert(ratio_task_snapshot.find("\"last_rebuild_status\":-5") != std::string::npos);
    assert(ratio_task_snapshot.find("\"last_rebuild_reason\":\"manual\"") != std::string::npos);
    assert(ratio_task_snapshot.find("\"last_rebuild_key\":\"svc-rebuild\"") != std::string::npos);

    std::string ratio_series_snapshot;
    assert(ratio_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-rebuild", 11},
                                               &ratio_series_snapshot) == error::OK);
    assert(ratio_series_snapshot.find("\"formal_ready\":false") != std::string::npos);
    assert(ratio_series_snapshot.find("\"formal_model_version\":0") != std::string::npos);
    assert(ratio_series_snapshot.find("\"candidate_generation\":0") != std::string::npos);
    assert(ratio_series_snapshot.find("\"candidate_state\":\"fetch_failed\"") != std::string::npos);
    assert(ratio_series_snapshot.find("\"last_rebuild_bucket_start\":0") != std::string::npos);
    assert(ratio_series_snapshot.find("\"last_rebuild_bucket_end\":500") != std::string::npos);
    assert(ratio_series_snapshot.find("\"last_replay_observation_count\":0") != std::string::npos);

    DetectorResult post_failure_ratio{};
    assert(ratio_task->SubmitObservation(
               RatioObservation{BaselineStringRef{"svc-rebuild", 11}, 501, 30.0, 40.0},
               &post_failure_ratio) == error::OK);
    assert(post_failure_ratio.status == error::OK);

    assert(value_task->Close() == error::OK);
    assert(ratio_task->Close() == error::OK);

    std::printf("[PASS] Baseline rebuild infrastructure\n");
}

static void TestBaselineFormalPredictorSkeleton() {
    std::printf("[TEST] Baseline formal predictor skeleton...\n");

    auto env = LoadBaselineService();
    auto* service = env.service;

    const char* value_cfg =
        R"({"name":"bytes_total","key":"service","feature":"bytes_total","feature_type":"t1a","feature_profile":"traffic","delta":60,"tz":"Asia/Shanghai"})";
    const char* ratio_cfg =
        R"({"name":"success_rate","key":"service","feature":"success_rate","feature_type":"t2","feature_profile":"rate_core","delta":60,"tz":"Asia/Shanghai"})";

    IBaselineValueTask* value_task = nullptr;
    IBaselineRatioTask* ratio_task = nullptr;
    assert(service->CreateValueTask(value_cfg, &value_task) == error::OK);
    assert(service->CreateRatioTask(ratio_cfg, &ratio_task) == error::OK);
    assert(value_task != nullptr);
    assert(ratio_task != nullptr);

    CountingValueHistoryReader value_reader;
    CountingRatioHistoryReader ratio_reader;
    assert(value_task->SetHistoryReader(&value_reader) == error::OK);
    assert(ratio_task->SetHistoryReader(&ratio_reader) == error::OK);

    DetectorResult value_result{};
    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-predict", 11}, 700, 64.0, 0},
               &value_result) == error::OK);
    DetectorResult ratio_result{};
    assert(ratio_task->SubmitObservation(
               RatioObservation{BaselineStringRef{"svc-predict", 11}, 800, 9.0, 10.0},
               &ratio_result) == error::OK);

    assert(value_task->RequestRebuild(BaselineStringRef{"svc-predict", 11},
                                      BaselineRebuildReason::kManual) == error::OK);
    assert(ratio_task->RequestRebuild(BaselineStringRef{"svc-predict", 11},
                                      BaselineRebuildReason::kManual) == error::OK);

    assert(WaitUntil([&value_reader]() { return value_reader.call_count() == 1; }));
    assert(WaitUntil([&ratio_reader]() { return ratio_reader.call_count() == 1; }));
    assert(WaitUntil([&value_task]() {
        std::string snapshot;
        if (value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-predict", 11},
                                                &snapshot) != error::OK) {
            return false;
        }
        auto doc = ParseJson(snapshot);
        return doc["formal_ready"].GetBool() == true;
    }));
    assert(WaitUntil([&ratio_task]() {
        std::string snapshot;
        if (ratio_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-predict", 11},
                                                &snapshot) != error::OK) {
            return false;
        }
        auto doc = ParseJson(snapshot);
        return doc["formal_ready"].GetBool() == true;
    }));

    std::string value_series_snapshot;
    assert(value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-predict", 11},
                                               &value_series_snapshot) == error::OK);
    auto value_doc = ParseJson(value_series_snapshot);
    assert(value_doc["formal_ready"].GetBool() == true);
    assert(std::string(value_doc["formal_model_kind"].GetString()) == "value_baseline");
    assert(value_doc["formal_model_version"].GetUint64() == 1);
    assert(value_doc["formal_predict_ready"].GetBool() == true);
    assert(value_doc["formal_predict_bucket_id"].GetInt64() == 700);
    assert(value_doc["formal_predict_value"].GetDouble() > 0.0);
    assert(value_doc["formal_predict_sigma_ref"].GetDouble() >= 1e-3);
    assert(value_doc["candidate_generation"].GetUint64() == 1);
    assert(value_doc["candidate_model_version"].GetUint64() == 0);
    assert(std::string(value_doc["candidate_model_kind"].GetString()) == "none");
    assert(std::string(value_doc["candidate_state"].GetString()) == "none");
    assert(std::string(value_doc["switch_state"].GetString()) == "direct_apply");
    assert(value_doc["candidate_predict_ready"].GetBool() == false);
    AssertDoubleNear(value_doc["candidate_predict_sigma_ref"].GetDouble(), 0.0);
    assert(value_doc["last_train_observation_count"].GetUint64() == 3);
    assert(value_doc["last_holdout_observation_count"].GetUint64() == 0);

    std::string ratio_series_snapshot;
    assert(ratio_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-predict", 11},
                                               &ratio_series_snapshot) == error::OK);
    auto ratio_doc = ParseJson(ratio_series_snapshot);
    assert(ratio_doc["formal_ready"].GetBool() == true);
    assert(std::string(ratio_doc["formal_model_kind"].GetString()) == "ratio_baseline");
    assert(ratio_doc["formal_model_version"].GetUint64() == 1);
    assert(ratio_doc["formal_predict_ready"].GetBool() == true);
    assert(ratio_doc["formal_predict_bucket_id"].GetInt64() == 800);
    assert(ratio_doc["formal_predict_value"].GetDouble() > 0.0);
    assert(ratio_doc["formal_predict_value"].GetDouble() < 1.0);
    assert(ratio_doc["candidate_generation"].GetUint64() == 1);
    assert(ratio_doc["candidate_model_version"].GetUint64() == 0);
    assert(std::string(ratio_doc["candidate_model_kind"].GetString()) == "none");
    assert(std::string(ratio_doc["candidate_state"].GetString()) == "none");
    assert(std::string(ratio_doc["switch_state"].GetString()) == "direct_apply");
    assert(ratio_doc["candidate_predict_ready"].GetBool() == false);
    assert(ratio_doc["last_train_observation_count"].GetUint64() == 3);
    assert(ratio_doc["last_holdout_observation_count"].GetUint64() == 0);

    assert(value_task->Close() == error::OK);
    assert(ratio_task->Close() == error::OK);

    std::printf("[PASS] Baseline formal predictor skeleton\n");
}

static void TestBaselineFormalTrainerFailureReason() {
    std::printf("[TEST] Baseline formal trainer failure reason...\n");

    auto env = LoadBaselineService();
    auto* service = env.service;

    const char* value_cfg =
        R"({"name":"bytes_total","key":"service","feature":"bytes_total","feature_type":"t1a","feature_profile":"traffic","delta":60,"tz":"Asia/Shanghai"})";

    IBaselineValueTask* value_task = nullptr;
    assert(service->CreateValueTask(value_cfg, &value_task) == error::OK);
    assert(value_task != nullptr);

    SinglePointValueHistoryReader value_reader;
    assert(value_task->SetHistoryReader(&value_reader) == error::OK);

    DetectorResult value_result{};
    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-single", 10}, 900, 12.0, 0},
               &value_result) == error::OK);

    assert(value_task->RequestRebuild(BaselineStringRef{"svc-single", 10},
                                      BaselineRebuildReason::kManual) == error::OK);
    assert(WaitUntil([&value_reader]() { return value_reader.call_count() == 1; }));
    assert(WaitUntil([&value_task]() {
        std::string snapshot;
        if (value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-single", 10},
                                                &snapshot) != error::OK) {
            return false;
        }
        auto doc = ParseJson(snapshot);
        return std::string(doc["candidate_state"].GetString()) == "insufficient_train_data";
    }));

    std::string series_snapshot;
    assert(value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-single", 10},
                                               &series_snapshot) == error::OK);
    auto doc = ParseJson(series_snapshot);
    assert(doc["candidate_generation"].GetUint64() == 0);
    assert(doc["candidate_model_version"].GetUint64() == 0);
    assert(std::string(doc["candidate_model_kind"].GetString()) == "none");
    assert(std::string(doc["candidate_state"].GetString()) == "insufficient_train_data");
    assert(doc["candidate_predict_ready"].GetBool() == false);
    assert(doc["last_replay_observation_count"].GetUint64() == 1);
    assert(doc["last_train_observation_count"].GetUint64() == 0);
    assert(doc["last_holdout_observation_count"].GetUint64() == 0);

    assert(value_task->Close() == error::OK);

    std::printf("[PASS] Baseline formal trainer failure reason\n");
}

static void TestBaselineSourceSelectionWithFormalModel() {
    std::printf("[TEST] Baseline source selection with formal model...\n");

    auto env = LoadBaselineService();
    auto* service = env.service;

    const char* value_cfg =
        R"({"name":"bytes_total","key":"service","feature":"bytes_total","feature_type":"t1a","feature_profile":"traffic","delta":60,"tz":"Asia/Shanghai","baseline_source_configs":[{"key":"svc-target","baseline_sources":[{"source_key":"svc-source"}]}]})";
    const char* ratio_cfg =
        R"({"name":"success_rate","key":"service","feature":"success_rate","feature_type":"t2","feature_profile":"rate_core","delta":60,"tz":"Asia/Shanghai","baseline_source_configs":[{"key":"svc-target","baseline_sources":[{"source_key":"svc-source"}]}]})";

    IBaselineValueTask* value_task = nullptr;
    IBaselineRatioTask* ratio_task = nullptr;
    assert(service->CreateValueTask(value_cfg, &value_task) == error::OK);
    assert(service->CreateRatioTask(ratio_cfg, &ratio_task) == error::OK);
    assert(value_task != nullptr);
    assert(ratio_task != nullptr);

    std::string value_task_snapshot;
    assert(value_task->QueryTaskSnapshotJson(&value_task_snapshot) == error::OK);
    auto value_task_doc = ParseJson(value_task_snapshot);
    assert(value_task_doc["baseline_source_config_count"].GetUint64() == 1);

    std::string ratio_task_snapshot;
    assert(ratio_task->QueryTaskSnapshotJson(&ratio_task_snapshot) == error::OK);
    auto ratio_task_doc = ParseJson(ratio_task_snapshot);
    assert(ratio_task_doc["baseline_source_config_count"].GetUint64() == 1);

    CountingValueHistoryReader value_reader;
    CountingRatioHistoryReader ratio_reader;
    assert(value_task->SetHistoryReader(&value_reader) == error::OK);
    assert(ratio_task->SetHistoryReader(&ratio_reader) == error::OK);

    DetectorResult source_value_result{};
    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-source", 10}, 1000, 64.0, 0},
               &source_value_result) == error::OK);
    DetectorResult source_ratio_result{};
    assert(ratio_task->SubmitObservation(
               RatioObservation{BaselineStringRef{"svc-source", 10}, 1000, 9.0, 10.0},
               &source_ratio_result) == error::OK);

    assert(value_task->RequestRebuild(BaselineStringRef{"svc-source", 10},
                                      BaselineRebuildReason::kManual) == error::OK);
    assert(ratio_task->RequestRebuild(BaselineStringRef{"svc-source", 10},
                                      BaselineRebuildReason::kManual) == error::OK);

    assert(WaitUntil([&value_reader]() { return value_reader.call_count() == 1; }));
    assert(WaitUntil([&ratio_reader]() { return ratio_reader.call_count() == 1; }));
    assert(WaitUntil([&value_task]() {
        std::string snapshot;
        if (value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-source", 10},
                                                &snapshot) != error::OK) {
            return false;
        }
        auto doc = ParseJson(snapshot);
        return doc["formal_ready"].GetBool() == true;
    }));
    assert(WaitUntil([&ratio_task]() {
        std::string snapshot;
        if (ratio_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-source", 10},
                                                &snapshot) != error::OK) {
            return false;
        }
        auto doc = ParseJson(snapshot);
        return doc["formal_ready"].GetBool() == true;
    }));

    DetectorResult unconfigured_value_result{};
    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-none", 8}, 1001, 32.0, 0},
               &unconfigured_value_result) == error::OK);
    assert(unconfigured_value_result.provider != BaselineProvider::kSource);

    DetectorResult unconfigured_ratio_result{};
    assert(ratio_task->SubmitObservation(
               RatioObservation{BaselineStringRef{"svc-none", 8}, 1001, 7.0, 10.0},
               &unconfigured_ratio_result) == error::OK);
    assert(unconfigured_ratio_result.provider != BaselineProvider::kSource);

    DetectorResult target_value_result{};
    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-target", 10}, 1001, 32.0, 0},
               &target_value_result) == error::OK);
    assert(target_value_result.provider == BaselineProvider::kSource);

    DetectorResult target_ratio_result{};
    assert(ratio_task->SubmitObservation(
               RatioObservation{BaselineStringRef{"svc-target", 10}, 1001, 7.0, 10.0},
               &target_ratio_result) == error::OK);
    assert(target_ratio_result.provider == BaselineProvider::kSource);

    std::string value_series_snapshot;
    std::string value_source_snapshot;
    assert(value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-source", 10},
                                               &value_source_snapshot) == error::OK);
    auto value_source_doc = ParseJson(value_source_snapshot);
    assert(value_source_doc["formal_ready"].GetBool() == true);
    assert(value_source_doc["formal_model_version"].GetUint64() == 1);

    assert(value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-target", 10},
                                               &value_series_snapshot) == error::OK);
    auto value_doc = ParseJson(value_series_snapshot);
    assert(std::string(value_doc["baseline_source_kind"].GetString()) == "configured_source");
    assert(std::string(value_doc["baseline_source_key"].GetString()) == "svc-source");
    assert(std::string(value_doc["model_state"].GetString()) == "serviceable_source");

    std::string value_none_snapshot;
    assert(value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-none", 8},
                                               &value_none_snapshot) == error::OK);
    auto value_none_doc = ParseJson(value_none_snapshot);
    assert(std::string(value_none_doc["baseline_source_kind"].GetString()) == "none");

    std::string ratio_series_snapshot;
    std::string ratio_source_snapshot;
    assert(ratio_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-source", 10},
                                               &ratio_source_snapshot) == error::OK);
    auto ratio_source_doc = ParseJson(ratio_source_snapshot);
    assert(ratio_source_doc["formal_ready"].GetBool() == true);
    assert(ratio_source_doc["formal_model_version"].GetUint64() == 1);

    assert(ratio_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-target", 10},
                                               &ratio_series_snapshot) == error::OK);
    auto ratio_doc = ParseJson(ratio_series_snapshot);
    assert(std::string(ratio_doc["baseline_source_kind"].GetString()) == "configured_source");
    assert(std::string(ratio_doc["baseline_source_key"].GetString()) == "svc-source");
    assert(std::string(ratio_doc["model_state"].GetString()) == "serviceable_source");

    std::string ratio_none_snapshot;
    assert(ratio_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-none", 8},
                                               &ratio_none_snapshot) == error::OK);
    auto ratio_none_doc = ParseJson(ratio_none_snapshot);
    assert(std::string(ratio_none_doc["baseline_source_kind"].GetString()) == "none");

    assert(value_task->Close() == error::OK);
    assert(ratio_task->Close() == error::OK);

    std::printf("[PASS] Baseline source selection with formal model\n");
}

static void TestBaselineShadowBaselineAndFormalSwitch() {
    std::printf("[TEST] Baseline shadow baseline and formal switch...\n");

    auto env = LoadBaselineService();
    auto* service = env.service;

    const char* value_cfg =
        R"({"name":"bytes_total","key":"service","feature":"bytes_total","feature_type":"t1a","feature_profile":"traffic","delta":60,"tz":"Asia/Shanghai"})";
    const char* ratio_cfg =
        R"({"name":"success_rate","key":"service","feature":"success_rate","feature_type":"t2","feature_profile":"rate_core","delta":60,"tz":"Asia/Shanghai"})";

    IBaselineValueTask* value_task = nullptr;
    IBaselineRatioTask* ratio_task = nullptr;
    assert(service->CreateValueTask(value_cfg, &value_task) == error::OK);
    assert(service->CreateRatioTask(ratio_cfg, &ratio_task) == error::OK);
    assert(value_task != nullptr);
    assert(ratio_task != nullptr);

    SwitchingValueHistoryReader value_reader;
    SwitchingRatioHistoryReader ratio_reader;
    assert(value_task->SetHistoryReader(&value_reader) == error::OK);
    assert(ratio_task->SetHistoryReader(&ratio_reader) == error::OK);

    DetectorResult warmup_value{};
    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-switch", 10}, 200, 1.0, 0},
               &warmup_value) == error::OK);
    DetectorResult warmup_ratio{};
    assert(ratio_task->SubmitObservation(
               RatioObservation{BaselineStringRef{"svc-switch", 10}, 200, 10.0, 100.0},
               &warmup_ratio) == error::OK);

    assert(value_task->RequestRebuild(BaselineStringRef{"svc-switch", 10},
                                      BaselineRebuildReason::kManual) == error::OK);
    assert(ratio_task->RequestRebuild(BaselineStringRef{"svc-switch", 10},
                                      BaselineRebuildReason::kManual) == error::OK);
    assert(WaitUntil([&value_reader]() { return value_reader.call_count() == 1; }));
    assert(WaitUntil([&ratio_reader]() { return ratio_reader.call_count() == 1; }));
    assert(WaitUntil([&value_task]() {
        std::string snapshot;
        if (value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-switch", 10},
                                                &snapshot) != error::OK) {
            return false;
        }
        auto doc = ParseJson(snapshot);
        return doc["formal_ready"].GetBool() == true;
    }));
    assert(WaitUntil([&ratio_task]() {
        std::string snapshot;
        if (ratio_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-switch", 10},
                                                &snapshot) != error::OK) {
            return false;
        }
        auto doc = ParseJson(snapshot);
        return doc["formal_ready"].GetBool() == true;
    }));

    std::string value_initial_snapshot;
    assert(value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-switch", 10},
                                               &value_initial_snapshot) == error::OK);
    auto value_initial_doc = ParseJson(value_initial_snapshot);
    assert(value_initial_doc["formal_ready"].GetBool() == true);
    assert(value_initial_doc["formal_model_version"].GetUint64() == 1);

    std::string ratio_initial_snapshot;
    assert(ratio_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-switch", 10},
                                               &ratio_initial_snapshot) == error::OK);
    auto ratio_initial_doc = ParseJson(ratio_initial_snapshot);
    assert(ratio_initial_doc["formal_ready"].GetBool() == true);
    assert(ratio_initial_doc["formal_model_version"].GetUint64() == 1);

    DetectorResult value_shift_1{};
    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-switch", 10}, 201, 64.0, 0},
               &value_shift_1) == error::OK);
    assert((value_shift_1.flags & kBaselineFlagShadowActive) == 0);

    DetectorResult value_shift_2{};
    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-switch", 10}, 202, 64.0, 0},
               &value_shift_2) == error::OK);
    assert((value_shift_2.flags & kBaselineFlagShadowActive) == 0);

    DetectorResult value_shift_3{};
    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-switch", 10}, 203, 64.0, 0},
               &value_shift_3) == error::OK);
    assert(value_shift_3.provider == BaselineProvider::kShadow);
    assert((value_shift_3.flags & kBaselineFlagShadowActive) != 0);
    assert((value_shift_3.flags & kBaselineFlagRebuildQueued) != 0);

    DetectorResult ratio_shift_1{};
    assert(ratio_task->SubmitObservation(
               RatioObservation{BaselineStringRef{"svc-switch", 10}, 201, 23.0, 100.0},
               &ratio_shift_1) == error::OK);
    assert((ratio_shift_1.flags & kBaselineFlagShadowActive) == 0);

    DetectorResult ratio_shift_2{};
    assert(ratio_task->SubmitObservation(
               RatioObservation{BaselineStringRef{"svc-switch", 10}, 202, 23.0, 100.0},
               &ratio_shift_2) == error::OK);
    assert((ratio_shift_2.flags & kBaselineFlagShadowActive) == 0);

    DetectorResult ratio_shift_3{};
    assert(ratio_task->SubmitObservation(
               RatioObservation{BaselineStringRef{"svc-switch", 10}, 203, 23.0, 100.0},
               &ratio_shift_3) == error::OK);
    assert(ratio_shift_3.provider == BaselineProvider::kShadow);
    assert((ratio_shift_3.flags & kBaselineFlagShadowActive) != 0);
    assert((ratio_shift_3.flags & kBaselineFlagRebuildQueued) != 0);

    assert(WaitUntil([&value_reader]() { return value_reader.second_fetch_started(); }));

    std::string value_shadow_snapshot;
    assert(value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-switch", 10},
                                               &value_shadow_snapshot) == error::OK);
    auto value_shadow_doc = ParseJson(value_shadow_snapshot);
    assert(value_shadow_doc["shadow_active"].GetBool() == true);
    assert(value_shadow_doc["formal_model_version"].GetUint64() == 1);
    assert(std::string(value_shadow_doc["model_state"].GetString()) == "shadow_self");
    assert(std::string(value_shadow_doc["shadow_ref_kind"].GetString()) == "self_formal");
    assert(value_shadow_doc["shadow_ref_model_version"].GetUint64() == 1);

    std::string ratio_shadow_snapshot;
    assert(ratio_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-switch", 10},
                                               &ratio_shadow_snapshot) == error::OK);
    auto ratio_shadow_doc = ParseJson(ratio_shadow_snapshot);
    assert(ratio_shadow_doc["shadow_active"].GetBool() == true);
    assert(ratio_shadow_doc["formal_model_version"].GetUint64() == 1);
    assert(std::string(ratio_shadow_doc["model_state"].GetString()) == "shadow_self");
    assert(std::string(ratio_shadow_doc["shadow_ref_kind"].GetString()) == "self_formal");
    assert(ratio_shadow_doc["shadow_ref_model_version"].GetUint64() == 1);

    value_reader.AllowSecondFetch();
    assert(WaitUntil([&ratio_reader]() { return ratio_reader.second_fetch_started(); }));
    ratio_reader.AllowSecondFetch();

    assert(WaitUntil([&]() {
        std::string snapshot;
        if (value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-switch", 10}, &snapshot) != error::OK) {
            return false;
        }
        auto doc = ParseJson(snapshot);
        return doc["shadow_active"].GetBool() == true;
    }));

    assert(WaitUntil([&]() {
        std::string snapshot;
        if (ratio_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-switch", 10}, &snapshot) != error::OK) {
            return false;
        }
        auto doc = ParseJson(snapshot);
        return doc["shadow_active"].GetBool() == true;
    }));

    for (int64_t bucket = 204; bucket <= 240; ++bucket) {
        DetectorResult value_bridge{};
        assert(value_task->SubmitObservation(
                   ValueObservation{BaselineStringRef{"svc-switch", 10}, bucket, 64.0, 0},
                   &value_bridge) == error::OK);
        DetectorResult ratio_bridge{};
        assert(ratio_task->SubmitObservation(
                   RatioObservation{BaselineStringRef{"svc-switch", 10}, bucket, 23.0, 100.0},
                   &ratio_bridge) == error::OK);
    }

    assert(value_task->RequestRebuild(BaselineStringRef{"svc-switch", 10},
                                      BaselineRebuildReason::kManual) == error::OK);
    assert(ratio_task->RequestRebuild(BaselineStringRef{"svc-switch", 10},
                                      BaselineRebuildReason::kManual) == error::OK);
    assert(WaitUntil([&value_reader]() { return value_reader.call_count() == 3; }));
    assert(WaitUntil([&ratio_reader]() { return ratio_reader.call_count() == 3; }));

    std::string value_final_snapshot;
    const bool value_switched = WaitUntil([&]() {
        if (value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-switch", 10},
                                                &value_final_snapshot) != error::OK) {
            return false;
        }
        auto doc = ParseJson(value_final_snapshot);
        return doc["formal_ready"].GetBool() &&
               doc["formal_model_version"].GetUint64() >= 2 &&
               doc["shadow_active"].GetBool() == false &&
               std::string(doc["switch_state"].GetString()) == "formal_apply";
    });
    if (!value_switched) {
        std::fprintf(stderr, "[DEBUG] value switch snapshot: %s\n",
                     value_final_snapshot.c_str());
    }
    assert(value_switched);

    std::string ratio_final_snapshot;
    const bool ratio_switched = WaitUntil([&]() {
        if (ratio_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-switch", 10},
                                                &ratio_final_snapshot) != error::OK) {
            return false;
        }
        auto doc = ParseJson(ratio_final_snapshot);
        return doc["formal_ready"].GetBool() &&
               doc["formal_model_version"].GetUint64() >= 2 &&
               doc["shadow_active"].GetBool() == false &&
               std::string(doc["switch_state"].GetString()) == "formal_apply";
    });
    if (!ratio_switched) {
        std::fprintf(stderr, "[DEBUG] ratio switch snapshot: %s\n",
                     ratio_final_snapshot.c_str());
    }
    assert(ratio_switched);

    DetectorResult value_after_switch{};
    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-switch", 10}, 241, 64.0, 0},
               &value_after_switch) == error::OK);
    assert(value_after_switch.provider == BaselineProvider::kFormal);
    assert((value_after_switch.flags & kBaselineFlagShadowActive) == 0);
    assert(value_after_switch.raw_score < 0.05);

    DetectorResult ratio_after_switch{};
    assert(ratio_task->SubmitObservation(
               RatioObservation{BaselineStringRef{"svc-switch", 10}, 241, 23.0, 100.0},
               &ratio_after_switch) == error::OK);
    assert(ratio_after_switch.provider == BaselineProvider::kFormal);
    assert((ratio_after_switch.flags & kBaselineFlagShadowActive) == 0);
    assert(ratio_after_switch.raw_score < 0.05);

    assert(value_task->Close() == error::OK);
    assert(ratio_task->Close() == error::OK);

    std::printf("[PASS] Baseline shadow baseline and formal switch\n");
}

static void TestBaselineMainScoringChain() {
    std::printf("[TEST] Baseline main scoring chain...\n");

    auto env = LoadBaselineService();
    auto* service = env.service;

    const char* value_cfg =
        R"({"name":"bytes_total","key":"service","feature":"bytes_total","feature_type":"t1a","feature_profile":"traffic","delta":60,"tz":"Asia/Shanghai","baseline_source_configs":[{"key":"svc-source-target","baseline_sources":[{"source_key":"svc-source"}]}]})";
    const char* ratio_cfg =
        R"({"name":"success_rate","key":"service","feature":"success_rate","feature_type":"t2","feature_profile":"rate_core","delta":60,"tz":"Asia/Shanghai","baseline_source_configs":[{"key":"svc-source-target","baseline_sources":[{"source_key":"svc-source"}]}]})";

    IBaselineValueTask* value_task = nullptr;
    IBaselineRatioTask* ratio_task = nullptr;
    assert(service->CreateValueTask(value_cfg, &value_task) == error::OK);
    assert(service->CreateRatioTask(ratio_cfg, &ratio_task) == error::OK);
    assert(value_task != nullptr);
    assert(ratio_task != nullptr);

    CountingValueHistoryReader value_reader;
    CountingRatioHistoryReader ratio_reader;
    assert(value_task->SetHistoryReader(&value_reader) == error::OK);
    assert(ratio_task->SetHistoryReader(&ratio_reader) == error::OK);

    DetectorResult warmup_value_result{};
    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-self", 8}, 1100, 16.0, 0},
               &warmup_value_result) == error::OK);
    DetectorResult warmup_ratio_result{};
    assert(ratio_task->SubmitObservation(
               RatioObservation{BaselineStringRef{"svc-self", 8}, 1100, 9.0, 10.0},
               &warmup_ratio_result) == error::OK);

    DetectorResult source_value_warmup{};
    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-source", 10}, 1100, 16.0, 0},
               &source_value_warmup) == error::OK);
    DetectorResult source_ratio_warmup{};
    assert(ratio_task->SubmitObservation(
               RatioObservation{BaselineStringRef{"svc-source", 10}, 1100, 9.0, 10.0},
               &source_ratio_warmup) == error::OK);

    assert(value_task->RequestRebuild(BaselineStringRef{"svc-self", 8},
                                      BaselineRebuildReason::kManual) == error::OK);
    assert(ratio_task->RequestRebuild(BaselineStringRef{"svc-self", 8},
                                      BaselineRebuildReason::kManual) == error::OK);
    assert(value_task->RequestRebuild(BaselineStringRef{"svc-source", 10},
                                      BaselineRebuildReason::kManual) == error::OK);
    assert(ratio_task->RequestRebuild(BaselineStringRef{"svc-source", 10},
                                      BaselineRebuildReason::kManual) == error::OK);

    assert(WaitUntil([&value_reader]() { return value_reader.call_count() == 2; }));
    assert(WaitUntil([&ratio_reader]() { return ratio_reader.call_count() == 2; }));
    assert(WaitUntil([&value_task]() {
        std::string snapshot;
        if (value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-self", 8},
                                                &snapshot) != error::OK) {
            return false;
        }
        auto doc = ParseJson(snapshot);
        return doc["formal_ready"].GetBool() == true;
    }));
    assert(WaitUntil([&ratio_task]() {
        std::string snapshot;
        if (ratio_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-self", 8},
                                                &snapshot) != error::OK) {
            return false;
        }
        auto doc = ParseJson(snapshot);
        return doc["formal_ready"].GetBool() == true;
    }));
    assert(WaitUntil([&value_task]() {
        std::string snapshot;
        if (value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-source", 10},
                                                &snapshot) != error::OK) {
            return false;
        }
        auto doc = ParseJson(snapshot);
        return doc["formal_ready"].GetBool() == true;
    }));
    assert(WaitUntil([&ratio_task]() {
        std::string snapshot;
        if (ratio_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-source", 10},
                                                &snapshot) != error::OK) {
            return false;
        }
        auto doc = ParseJson(snapshot);
        return doc["formal_ready"].GetBool() == true;
    }));

    DetectorResult self_value_result{};
    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-self", 8}, 1101, 64.0, 0},
               &self_value_result) == error::OK);
    assert(self_value_result.provider == BaselineProvider::kFormal);
    assert(self_value_result.raw_score > 0.0);
    assert(self_value_result.normalized_score > 0.0);
    assert(self_value_result.confidence > 0.0);
    assert(self_value_result.reason_code == BaselineReasonCode::kBaselineShiftUp);

    DetectorResult self_ratio_result{};
    assert(ratio_task->SubmitObservation(
               RatioObservation{BaselineStringRef{"svc-self", 8}, 1101, 60.0, 60.0},
               &self_ratio_result) == error::OK);
    assert(self_ratio_result.provider == BaselineProvider::kFormal);
    assert(self_ratio_result.raw_score > 0.0);
    assert(self_ratio_result.normalized_score > 0.0);
    assert(self_ratio_result.confidence > 0.0);
    assert(self_ratio_result.reason_code == BaselineReasonCode::kSpike);

    DetectorResult source_value_result{};
    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-source-target", 17}, 1101, 64.0, 0},
               &source_value_result) == error::OK);
    assert(source_value_result.provider == BaselineProvider::kSource);
    AssertDoubleNear(source_value_result.raw_score, self_value_result.raw_score);
    assert(source_value_result.normalized_score > 0.0);
    assert(source_value_result.confidence > 0.0);
    assert(source_value_result.reason_code == BaselineReasonCode::kBaselineShiftUp);

    DetectorResult source_ratio_result{};
    assert(ratio_task->SubmitObservation(
               RatioObservation{BaselineStringRef{"svc-source-target", 17}, 1101, 0.0, 60.0},
               &source_ratio_result) == error::OK);
    assert(source_ratio_result.provider == BaselineProvider::kSource);
    assert(source_ratio_result.raw_score > 0.0);
    assert(source_ratio_result.normalized_score > 0.0);
    assert(source_ratio_result.confidence > 0.0);
    assert(source_ratio_result.reason_code == BaselineReasonCode::kDrop);

    DetectorResult none_value_result{};
    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-none", 8}, 1101, 64.0, 0},
               &none_value_result) == error::OK);
    assert(none_value_result.provider == BaselineProvider::kFormal);
    assert(none_value_result.raw_score == 0.0);
    assert(none_value_result.normalized_score == 0.0);
    assert(none_value_result.confidence == 0.0);
    assert(none_value_result.reason_code == BaselineReasonCode::kUnknown);

    DetectorResult none_ratio_result{};
    assert(ratio_task->SubmitObservation(
               RatioObservation{BaselineStringRef{"svc-none", 8}, 1101, 0.0, 60.0},
               &none_ratio_result) == error::OK);
    assert(none_ratio_result.provider == BaselineProvider::kNone);
    assert(none_ratio_result.raw_score == 0.0);
    assert(none_ratio_result.normalized_score == 0.0);
    assert(none_ratio_result.confidence == 0.0);
    assert(none_ratio_result.reason_code == BaselineReasonCode::kUnknown);

    assert(value_task->Close() == error::OK);
    assert(ratio_task->Close() == error::OK);

    std::printf("[PASS] Baseline main scoring chain\n");
}

static void TestBaselineEventCalendarConfigAndSnapshot() {
    std::printf("[TEST] Baseline event calendar config and snapshot...\n");

    auto env = LoadBaselineService();
    auto* service = env.service;

    const char* value_cfg =
        R"({"name":"bytes_total","key":"service","feature":"bytes_total","feature_type":"t1a","feature_profile":"traffic","delta":60,"tz":"Asia/Shanghai","event_calendar_spec":{"calendar_id":"ops-calendar","calendar_version":"2026.04","entries":[{"event_code":"month_close","scope_type":"global","alignment_mode":"local_wall_clock","start_ts":1711900800,"end_ts":1711987199,"enabled":true,"tz":"Asia/Shanghai"}]}})";
    const char* ratio_cfg =
        R"({"name":"success_rate","key":"service","feature":"success_rate","feature_type":"t2","feature_profile":"rate_core","delta":60,"tz":"Asia/Shanghai","event_calendar_spec":{"calendar_id":"ops-calendar","calendar_version":"2026.04","entries":[{"event_code":"month_close","scope_type":"global","alignment_mode":"local_wall_clock","start_ts":1711900800,"end_ts":1711987199,"enabled":true,"tz":"Asia/Shanghai"}]}})";
    const char* bad_value_cfg =
        R"({"name":"bytes_total","key":"service","feature":"bytes_total","feature_type":"t1a","feature_profile":"traffic","delta":60,"tz":"Asia/Shanghai","event_calendar_spec":{"calendar_id":"ops-calendar","calendar_version":"2026.04","entries":[{"event_code":"broken","scope_type":"global","alignment_mode":"absolute_utc","start_ts":1711987199,"end_ts":1711900800,"enabled":true}]}})";

    IBaselineValueTask* bad_value_task = reinterpret_cast<IBaselineValueTask*>(0x1);
    assert(service->CreateValueTask(bad_value_cfg, &bad_value_task) == error::BAD_REQUEST);
    assert(bad_value_task == nullptr);

    IBaselineValueTask* value_task = nullptr;
    IBaselineRatioTask* ratio_task = nullptr;
    assert(service->CreateValueTask(value_cfg, &value_task) == error::OK);
    assert(service->CreateRatioTask(ratio_cfg, &ratio_task) == error::OK);
    assert(value_task != nullptr);
    assert(ratio_task != nullptr);

    std::string value_task_snapshot;
    assert(value_task->QueryTaskSnapshotJson(&value_task_snapshot) == error::OK);
    auto value_task_doc = ParseJson(value_task_snapshot);
    assert(value_task_doc["event_calendar_present"].GetBool() == true);
    assert(std::string(value_task_doc["event_calendar_id"].GetString()) == "ops-calendar");
    assert(std::string(value_task_doc["event_calendar_version"].GetString()) == "2026.04");
    assert(value_task_doc["event_calendar_entry_count"].GetUint64() == 1);

    std::string ratio_task_snapshot;
    assert(ratio_task->QueryTaskSnapshotJson(&ratio_task_snapshot) == error::OK);
    auto ratio_task_doc = ParseJson(ratio_task_snapshot);
    assert(ratio_task_doc["event_calendar_present"].GetBool() == true);
    assert(std::string(ratio_task_doc["event_calendar_id"].GetString()) == "ops-calendar");
    assert(std::string(ratio_task_doc["event_calendar_version"].GetString()) == "2026.04");
    assert(ratio_task_doc["event_calendar_entry_count"].GetUint64() == 1);

    CountingValueHistoryReader value_reader;
    CountingRatioHistoryReader ratio_reader;
    assert(value_task->SetHistoryReader(&value_reader) == error::OK);
    assert(ratio_task->SetHistoryReader(&ratio_reader) == error::OK);

    DetectorResult value_result{};
    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-calendar", 12}, 1200, 64.0, 0},
               &value_result) == error::OK);
    DetectorResult ratio_result{};
    assert(ratio_task->SubmitObservation(
               RatioObservation{BaselineStringRef{"svc-calendar", 12}, 1200, 12.0, 20.0},
               &ratio_result) == error::OK);

    assert(value_task->RequestRebuild(BaselineStringRef{"svc-calendar", 12},
                                      BaselineRebuildReason::kManual) == error::OK);
    assert(ratio_task->RequestRebuild(BaselineStringRef{"svc-calendar", 12},
                                      BaselineRebuildReason::kManual) == error::OK);

    assert(WaitUntil([&value_reader]() { return value_reader.call_count() == 1; }));
    assert(WaitUntil([&ratio_reader]() { return ratio_reader.call_count() == 1; }));
    assert(WaitUntil([&value_task]() {
        std::string snapshot;
        if (value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-calendar", 12},
                                                &snapshot) != error::OK) {
            return false;
        }
        auto doc = ParseJson(snapshot);
        return std::string(doc["switch_state"].GetString()) == "direct_apply";
    }));
    assert(WaitUntil([&ratio_task]() {
        std::string snapshot;
        if (ratio_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-calendar", 12},
                                                &snapshot) != error::OK) {
            return false;
        }
        auto doc = ParseJson(snapshot);
        return std::string(doc["switch_state"].GetString()) == "direct_apply";
    }));

    std::string value_series_snapshot;
    assert(value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-calendar", 12},
                                               &value_series_snapshot) == error::OK);
    auto value_doc = ParseJson(value_series_snapshot);
    assert(std::string(value_doc["formal_calendar_id"].GetString()) == "ops-calendar");
    assert(std::string(value_doc["formal_calendar_version"].GetString()) == "2026.04");
    assert(value_doc["formal_event_enabled"].GetBool() == true);
    assert(std::string(value_doc["formal_event_status"].GetString()) == "enabled");
    assert(std::string(value_doc["switch_state"].GetString()) == "direct_apply");

    std::string ratio_series_snapshot;
    assert(ratio_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-calendar", 12},
                                               &ratio_series_snapshot) == error::OK);
    auto ratio_doc = ParseJson(ratio_series_snapshot);
    assert(std::string(ratio_doc["formal_calendar_id"].GetString()) == "ops-calendar");
    assert(std::string(ratio_doc["formal_calendar_version"].GetString()) == "2026.04");
    assert(ratio_doc["formal_event_enabled"].GetBool() == true);
    assert(std::string(ratio_doc["formal_event_status"].GetString()) == "enabled");
    assert(std::string(ratio_doc["switch_state"].GetString()) == "direct_apply");

    assert(value_task->Close() == error::OK);
    assert(ratio_task->Close() == error::OK);

    std::printf("[PASS] Baseline event calendar config and snapshot\n");
}

static void TestBaselineFormalPredictorEventCalendarContract() {
    std::printf("[TEST] Baseline formal predictor event calendar contract...\n");

    baseline::EventCalendarSpec task_calendar;
    task_calendar.calendar_id = "ops-calendar";
    task_calendar.calendar_version = "2026.04";

    baseline::FormalModelMetadata metadata;
    metadata.kind = baseline::FormalModelKind::kValueBaseline;
    metadata.model_version = 7;
    metadata.calendar_id = "ops-calendar";
    metadata.calendar_version = "2026.04";

    task_calendar.calendar_version = "2026.05";
    assert(baseline::EvaluateEventCalendarStatus(metadata, &task_calendar) ==
           baseline::EventCalendarStatus::kDisabledCalendarMismatch);

    assert(baseline::EvaluateEventCalendarStatus(
               metadata, static_cast<const baseline::EventCalendarSpec*>(nullptr)) ==
           baseline::EventCalendarStatus::kDisabledNoTaskCalendar);

    metadata.calendar_version = "";
    task_calendar.calendar_version = "2026.04";
    assert(baseline::EvaluateEventCalendarStatus(metadata, &task_calendar) ==
           baseline::EventCalendarStatus::kDisabledNoModelCalendar);

    metadata.calendar_version = "2026.04";
    assert(baseline::EvaluateEventCalendarStatus(metadata, &task_calendar) ==
           baseline::EventCalendarStatus::kEnabled);

    std::printf("[PASS] Baseline formal predictor event calendar contract\n");
}

static void TestBaselineKeyFusionSnapshotForValueTask() {
    std::printf("[TEST] Baseline key fusion snapshot for value task...\n");

    auto env = LoadBaselineService();
    auto* service = env.service;

    const char* value_cfg =
        R"({"name":"bytes_total","key":"svc-fusion-value","feature":"bytes_total","feature_type":"t1a","feature_profile":"traffic","delta":60,"tz":"Asia/Shanghai"})";

    IBaselineValueTask* value_task = nullptr;
    assert(service->CreateValueTask(value_cfg, &value_task) == error::OK);
    assert(value_task != nullptr);

    const std::string key = "svc-fusion-value";
    DetectorResult result{};
    assert(value_task->SubmitObservation(ValueObservation{StringRef(key), 100, 64.0, 0},
                                         &result) == error::OK);

    std::string snapshot;
    assert(service->QueryKeyFusionSnapshotJson(StringRef(key), &snapshot) == error::OK);
    auto doc = ParseJson(snapshot);
    assert(doc["available"].GetBool() == true);
    assert(doc.HasMember("active_window"));
    assert(doc["active_window"]["ts"].GetInt64() == 100);

    assert(value_task->Close() == error::OK);

    assert(service->QueryKeyFusionSnapshotJson(StringRef(key), &snapshot) == error::OK);
    doc = ParseJson(snapshot);
    assert(doc["available"].GetBool() == false);

    std::printf("[PASS] Baseline key fusion snapshot for value task\n");
}

static void TestBaselineKeyFusionSnapshotForRelationTask() {
    std::printf("[TEST] Baseline key fusion snapshot for relation task...\n");

    auto env = LoadBaselineService();
    auto* service = env.service;

    const char* relation_cfg =
        R"({"name":"client_group_mix","feature_base":"client_group_mix","group_space_id":"client_group","group_space_version":"v1","delta":60,"tz":"Asia/Shanghai","metric_set_id":"net_metrics","metrics":["conn_count","bps"],"encode_type":"exact_sparse","support_policy":{"k_support":8,"min_hist_share":0.005,"min_active_ratio":0.2},"summary_policy":{"k_head":2,"k_stable":2}})";

    IBaselineRelationTask* relation_task = nullptr;
    assert(service->CreateRelationTask(relation_cfg, nullptr, &relation_task) == error::OK);
    assert(relation_task != nullptr);

    const std::string key = "svc-fusion-relation";
    const uint32_t group_idx[] = {11, 12, 50};
    const double conn_values_10[] = {50.0, 30.0, 20.0};
    const double bps_values_10[] = {400.0, 350.0, 250.0};
    const RelationMetricBlock metrics_10[] = {
        {100.0, kRelationMetricHasActiveCount, 3, conn_values_10},
        {1000.0, kRelationMetricHasActiveCount, 3, bps_values_10},
    };
    const RelationObservationBlock block_10{
        StringRef(key),
        10,
        3,
        group_idx,
        2,
        metrics_10,
    };

    const double conn_values_11[] = {48.0, 32.0, 20.0};
    const double bps_values_11[] = {390.0, 360.0, 250.0};
    const RelationMetricBlock metrics_11[] = {
        {100.0, kRelationMetricHasActiveCount, 3, conn_values_11},
        {1000.0, kRelationMetricHasActiveCount, 3, bps_values_11},
    };
    const RelationObservationBlock block_11{
        StringRef(key),
        11,
        3,
        group_idx,
        2,
        metrics_11,
    };

    FusionResult result_10{};
    FusionResult result_11{};
    assert(relation_task->SubmitBlock(block_10, &result_10) == error::OK);
    assert(relation_task->SubmitBlock(block_11, &result_11) == error::OK);

    std::string snapshot;
    assert(service->QueryKeyFusionSnapshotJson(StringRef(key), &snapshot) == error::OK);
    auto doc = ParseJson(snapshot);
    assert(doc["available"].GetBool() == true);
    assert(doc.HasMember("latest_finalized_result"));
    assert(doc["latest_finalized_result"]["ts"].GetInt64() == 10);
    AssertDoubleNear(doc["latest_finalized_result"]["risk"].GetDouble(), result_10.risk);
    assert(doc.HasMember("active_window"));
    assert(doc["active_window"]["ts"].GetInt64() == 11);
    AssertDoubleNear(doc["active_window"]["risk"].GetDouble(), result_11.risk);

    assert(relation_task->Close() == error::OK);

    assert(service->QueryKeyFusionSnapshotJson(StringRef(key), &snapshot) == error::OK);
    doc = ParseJson(snapshot);
    assert(doc["available"].GetBool() == false);

    std::printf("[PASS] Baseline key fusion snapshot for relation task\n");
}

int main() {
    TestBaselineServiceHeaderAndIid();
    TestBaselinePluginLoadAndQuery();
    TestBaselineTaskLifecycleAndConfigValidation();
    TestBaselineSeriesStoreCommonState();
    TestBaselineValueTaskHotPath();
    TestBaselineRatioTaskHotPath();
    TestBaselineRelationTaskHotPath();
    TestBaselineRelationTaskRebuildDirectApply();
    TestBaselineRelationTaskRebuildFormalApply();
    TestBaselineRebuildInfrastructure();
    TestBaselineFormalPredictorSkeleton();
    TestBaselineFormalTrainerFailureReason();
    TestBaselineSourceSelectionWithFormalModel();
    TestBaselineMainScoringChain();
    TestBaselineEventCalendarConfigAndSnapshot();
    TestBaselineFormalPredictorEventCalendarContract();
    TestBaselineShadowBaselineAndFormalSwitch();
    TestBaselineKeyFusionSnapshotForValueTask();
    TestBaselineKeyFusionSnapshotForRelationTask();
    std::printf("[DONE] test_baseline\n");
    return 0;
}
