/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <cassert>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <common/error_code.h>

#include "test_common.h"

using namespace flowsql;
using namespace flowsql::baseline_test;

namespace {

class BlockingValueHistoryReader : public IBaselineValueHistoryReader {
 public:
    int Fetch(const HistoryFetchRequest& req,
              std::function<int(const ValueObservation&)> on_point) override {
        const std::string key = req.key.data ? std::string(req.key.data, req.key.size) : "";
        {
            std::lock_guard<std::mutex> lock(mutex_);
            key_ = key;
            started_ = true;
        }
        cv_.notify_all();

        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return allow_; });
        lock.unlock();

        if (!on_point) return error::OK;
        for (int64_t bucket = 8; bucket <= 10; ++bucket) {
            const ValueObservation replay{
                BaselineStringRef{key_.c_str(), static_cast<uint32_t>(key_.size())},
                bucket,
                static_cast<double>(bucket),
                0};
            const int rc = on_point(replay);
            if (rc != error::OK) return rc;
        }
        return error::OK;
    }

    bool started() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return started_;
    }

    void Allow() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            allow_ = true;
        }
        cv_.notify_all();
    }

 private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string key_;
    bool started_ = false;
    bool allow_ = false;
};

void TestConcurrentSubmitAndSnapshotQueries() {
    std::printf("[TEST] Baseline concurrent submit and snapshot queries...\n");

    auto env = LoadBaselineService();
    auto* service = env.service;

    const char* value_cfg =
        R"({"name":"bytes_total","key":"service","feature":"bytes_total","feature_type":"t1a","feature_profile":"traffic","delta":60,"tz":"Asia/Shanghai"})";

    IBaselineValueTask* value_task = nullptr;
    assert(service->CreateValueTask(value_cfg, &value_task) == error::OK);

    DetectorResult warmup{};
    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-hot", 7}, 1, 1.0, 0},
               &warmup) == error::OK);

    std::atomic<bool> start{false};
    std::atomic<bool> stop_query{false};
    std::atomic<int> submit_error_count{0};
    std::atomic<int> query_error_count{0};
    std::vector<std::thread> workers;

    workers.emplace_back([&]() {
        while (!start.load()) std::this_thread::yield();
        for (int64_t bucket = 2; bucket <= 251; ++bucket) {
            DetectorResult result{};
            const int rc = value_task->SubmitObservation(
                ValueObservation{BaselineStringRef{"svc-hot", 7}, bucket,
                                 static_cast<double>(bucket), 0},
                &result);
            if (rc != error::OK) ++submit_error_count;
        }
    });

    for (int worker_index = 0; worker_index < 3; ++worker_index) {
        workers.emplace_back([&, worker_index]() {
            const std::string key = "svc-" + std::to_string(worker_index);
            while (!start.load()) std::this_thread::yield();
            for (int64_t bucket = 1; bucket <= 250; ++bucket) {
                DetectorResult result{};
                const int rc = value_task->SubmitObservation(
                    ValueObservation{BaselineStringRef{key.c_str(),
                                                      static_cast<uint32_t>(key.size())},
                                     bucket,
                                     static_cast<double>(bucket + worker_index + 1),
                                     0},
                    &result);
                if (rc != error::OK) ++submit_error_count;
            }
        });
    }

    std::thread query_thread([&]() {
        while (!start.load()) std::this_thread::yield();
        while (!stop_query.load()) {
            std::string task_snapshot;
            if (value_task->QueryTaskSnapshotJson(&task_snapshot) != error::OK) {
                ++query_error_count;
                continue;
            }

            std::string series_snapshot;
            const int rc = value_task->QuerySeriesSnapshotJson(
                BaselineStringRef{"svc-hot", 7}, &series_snapshot);
            if (rc != error::OK) {
                ++query_error_count;
                continue;
            }
            auto doc = ParseJson(series_snapshot);
            if (doc["observation_count"].GetUint64() == 0) ++query_error_count;
        }
    });

    start.store(true);
    for (auto& worker : workers) {
        worker.join();
    }
    stop_query.store(true);
    query_thread.join();

    assert(submit_error_count.load() == 0);
    assert(query_error_count.load() == 0);

    std::string hot_snapshot;
    assert(value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-hot", 7},
                                               &hot_snapshot) == error::OK);
    auto hot_doc = ParseJson(hot_snapshot);
    assert(hot_doc["observation_count"].GetUint64() == 251);
    assert(hot_doc["last_bucket_id"].GetInt64() == 251);

    assert(value_task->Close() == error::OK);
    std::printf("[PASS] Baseline concurrent submit and snapshot queries\n");
}

void TestCloseWaitsForInflightRebuild() {
    std::printf("[TEST] Baseline close waits for inflight rebuild...\n");

    auto env = LoadBaselineService();
    auto* service = env.service;

    const char* value_cfg =
        R"({"name":"bytes_total","key":"service","feature":"bytes_total","feature_type":"t1a","feature_profile":"traffic","delta":60,"tz":"Asia/Shanghai"})";

    IBaselineValueTask* value_task = nullptr;
    assert(service->CreateValueTask(value_cfg, &value_task) == error::OK);

    BlockingValueHistoryReader reader;
    assert(value_task->SetHistoryReader(&reader) == error::OK);

    DetectorResult result{};
    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-close", 9}, 10, 10.0, 0},
               &result) == error::OK);
    assert(value_task->RequestRebuild(BaselineStringRef{"svc-close", 9},
                                      BaselineRebuildReason::kManual) == error::OK);
    assert(WaitUntil([&]() { return reader.started(); }));

    std::atomic<bool> close_returned{false};
    int close_rc = error::UNAVAILABLE;
    std::thread close_thread([&]() {
        close_rc = value_task->Close();
        close_returned.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(close_returned.load() == false);

    reader.Allow();
    close_thread.join();
    assert(close_returned.load() == true);
    assert(close_rc == error::OK);

    std::printf("[PASS] Baseline close waits for inflight rebuild\n");
}

void TestPerformanceSmoke() {
    std::printf("[TEST] Baseline performance smoke...\n");

    auto env = LoadBaselineService();
    auto* service = env.service;

    const char* value_cfg =
        R"({"name":"bytes_total","key":"service","feature":"bytes_total","feature_type":"t1a","feature_profile":"traffic","delta":60,"tz":"Asia/Shanghai"})";

    IBaselineValueTask* value_task = nullptr;
    assert(service->CreateValueTask(value_cfg, &value_task) == error::OK);

    constexpr int kKeyCount = 64;
    constexpr int kObservationCount = 20000;
    std::vector<int64_t> next_bucket(static_cast<size_t>(kKeyCount), 1);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kObservationCount; ++i) {
        const int key_index = i % kKeyCount;
        const std::string key = "svc-perf-" + std::to_string(key_index);
        DetectorResult result{};
        const int rc = value_task->SubmitObservation(
            ValueObservation{BaselineStringRef{key.c_str(),
                                              static_cast<uint32_t>(key.size())},
                             next_bucket[static_cast<size_t>(key_index)]++,
                             static_cast<double>((i % 100) + 1),
                             0},
            &result);
        assert(rc == error::OK);
    }
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::string series_snapshot;
    assert(value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-perf-0", 10},
                                               &series_snapshot) == error::OK);
    auto doc = ParseJson(series_snapshot);
    assert(doc["observation_count"].GetUint64() > 0);

    std::printf("[INFO] Performance smoke: %d observations in %lld ms\n",
                kObservationCount, static_cast<long long>(elapsed_ms));

    assert(value_task->Close() == error::OK);
    std::printf("[PASS] Baseline performance smoke\n");
}

}  // namespace

int main() {
    TestConcurrentSubmitAndSnapshotQueries();
    TestCloseWaitsForInflightRebuild();
    TestPerformanceSmoke();
    std::printf("[DONE] test_baseline_concurrency\n");
    return 0;
}
