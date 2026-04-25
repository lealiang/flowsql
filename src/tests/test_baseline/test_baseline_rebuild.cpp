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
#include <vector>

#include <common/error_code.h>

#include "test_common.h"

using namespace flowsql;
using namespace flowsql::baseline_test;

namespace {

bool WaitForTaskCompleted(IBaselineTask* task, uint64_t expected_completed = 1) {
    return WaitUntil([&]() {
        std::string snapshot;
        if (task->QueryTaskSnapshotJson(&snapshot) != error::OK) return false;
        auto doc = ParseJson(snapshot);
        return doc["rebuild_completed"].GetUint64() >= expected_completed;
    });
}

class BlockingRelationHistoryReader : public IBaselineRelationHistoryReader {
 public:
    int Fetch(const HistoryFetchRequest& req,
              std::function<int(const RelationObservationBlock&)> on_block) override {
        ++call_count_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            key_ = req.key.data ? std::string(req.key.data, req.key.size) : "";
            started_ = true;
        }
        cv_.notify_all();

        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return allow_; });
        lock.unlock();

        const uint32_t group_idx[] = {11, 12};
        const double values[] = {70.0, 30.0};
        const RelationMetricBlock metric{100.0, 0, 2, values};
        const RelationObservationBlock block{
            BaselineStringRef{key_.c_str(), static_cast<uint32_t>(key_.size())},
            10,
            2,
            group_idx,
            1,
            &metric};
        return on_block ? on_block(block) : error::OK;
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
    std::atomic<int> call_count_{0};
    std::string key_;
    bool started_ = false;
    bool allow_ = false;
};

class DummyRelationHistoryReader : public IBaselineRelationHistoryReader {
 public:
    int Fetch(const HistoryFetchRequest&,
              std::function<int(const RelationObservationBlock&)>) override {
        return error::OK;
    }
};

void TestRebuildWithoutHistoryReader() {
    std::printf("[TEST] Baseline rebuild without history reader...\n");

    auto env = LoadBaselineService();
    auto* service = env.service;

    const char* value_cfg =
        R"({"name":"bytes_total","key":"service","feature":"bytes_total","feature_type":"t1a","feature_profile":"traffic","delta":60,"tz":"Asia/Shanghai"})";
    const char* ratio_cfg =
        R"({"name":"success_rate","key":"service","feature":"success_rate","feature_type":"t2","feature_profile":"rate_core","delta":60,"tz":"Asia/Shanghai"})";
    const char* relation_cfg =
        R"({"name":"client_group_mix","feature_base":"client_group_mix","group_space_id":"client_group","group_space_version":"v1","delta":60,"tz":"Asia/Shanghai","metric_set_id":"net_metrics","metrics":["conn_count"],"encode_type":"exact_sparse","support_policy":{"k_support":8,"min_hist_share":0.005,"min_active_ratio":0.2},"summary_policy":{"k_head":2,"k_stable":2}})";

    IBaselineValueTask* value_task = nullptr;
    IBaselineRatioTask* ratio_task = nullptr;
    IBaselineRelationTask* relation_task = nullptr;
    assert(service->CreateValueTask(value_cfg, &value_task) == error::OK);
    assert(service->CreateRatioTask(ratio_cfg, &ratio_task) == error::OK);
    assert(service->CreateRelationTask(relation_cfg, nullptr, &relation_task) == error::OK);

    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-no-reader", 13}, 10, 8.0, 0},
               nullptr) == error::BAD_REQUEST);

    DetectorResult value_result{};
    DetectorResult ratio_result{};
    FusionResult relation_result{};
    assert(value_task->SubmitObservation(
               ValueObservation{BaselineStringRef{"svc-no-reader", 13}, 10, 8.0, 0},
               &value_result) == error::OK);
    assert(ratio_task->SubmitObservation(
               RatioObservation{BaselineStringRef{"svc-no-reader", 13}, 10, 4.0, 5.0},
               &ratio_result) == error::OK);

    const uint32_t group_idx[] = {11, 12};
    const double values[] = {7.0, 3.0};
    const RelationMetricBlock metric{10.0, 0, 2, values};
    const RelationObservationBlock block{
        BaselineStringRef{"svc-no-reader", 13},
        10,
        2,
        group_idx,
        1,
        &metric};
    assert(relation_task->SubmitBlock(block, &relation_result) == error::OK);

    assert(value_task->RequestRebuild(BaselineStringRef{"svc-no-reader", 13},
                                      BaselineRebuildReason::kManual) == error::OK);
    assert(ratio_task->RequestRebuild(BaselineStringRef{"svc-no-reader", 13},
                                      BaselineRebuildReason::kManual) == error::OK);
    assert(relation_task->RequestRebuild(BaselineStringRef{"svc-no-reader", 13},
                                         BaselineRebuildReason::kManual) == error::OK);

    assert(WaitForTaskCompleted(value_task));
    assert(WaitForTaskCompleted(ratio_task));
    assert(WaitForTaskCompleted(relation_task));

    std::string value_task_snapshot;
    assert(value_task->QueryTaskSnapshotJson(&value_task_snapshot) == error::OK);
    auto value_task_doc = ParseJson(value_task_snapshot);
    assert(value_task_doc["last_rebuild_status"].GetInt() == error::UNAVAILABLE);

    std::string ratio_task_snapshot;
    assert(ratio_task->QueryTaskSnapshotJson(&ratio_task_snapshot) == error::OK);
    auto ratio_task_doc = ParseJson(ratio_task_snapshot);
    assert(ratio_task_doc["last_rebuild_status"].GetInt() == error::UNAVAILABLE);

    std::string relation_task_snapshot;
    assert(relation_task->QueryTaskSnapshotJson(&relation_task_snapshot) == error::OK);
    auto relation_task_doc = ParseJson(relation_task_snapshot);
    assert(relation_task_doc["last_rebuild_status"].GetInt() == error::UNAVAILABLE);

    std::string value_series_snapshot;
    assert(value_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-no-reader", 13},
                                               &value_series_snapshot) == error::OK);
    auto value_series_doc = ParseJson(value_series_snapshot);
    assert(std::string(value_series_doc["candidate_state"].GetString()) == "failed");
    assert(std::string(value_series_doc["switch_state"].GetString()) == "rebuild_blocked");
    assert(value_series_doc.HasMember("failure_reason"));
    assert(std::string(value_series_doc["failure_reason"].GetString()) == "unavailable");

    std::string ratio_series_snapshot;
    assert(ratio_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-no-reader", 13},
                                               &ratio_series_snapshot) == error::OK);
    auto ratio_series_doc = ParseJson(ratio_series_snapshot);
    assert(std::string(ratio_series_doc["candidate_state"].GetString()) == "failed");
    assert(std::string(ratio_series_doc["switch_state"].GetString()) == "rebuild_blocked");
    assert(ratio_series_doc.HasMember("failure_reason"));
    assert(std::string(ratio_series_doc["failure_reason"].GetString()) == "unavailable");

    std::string relation_series_snapshot;
    assert(relation_task->QuerySeriesSnapshotJson(BaselineStringRef{"svc-no-reader", 13},
                                                  &relation_series_snapshot) == error::OK);
    auto relation_series_doc = ParseJson(relation_series_snapshot);
    assert(std::string(relation_series_doc["candidate_state"].GetString()) == "failed");
    assert(std::string(relation_series_doc["switch_state"].GetString()) == "rebuild_blocked");
    assert(relation_series_doc.HasMember("failure_reason"));
    assert(std::string(relation_series_doc["failure_reason"].GetString()) == "unavailable");

    assert(value_task->Close() == error::OK);
    assert(ratio_task->Close() == error::OK);
    assert(relation_task->Close() == error::OK);

    std::printf("[PASS] Baseline rebuild without history reader\n");
}

void TestRelationReaderSwapConflictWhileRebuildInflight() {
    std::printf("[TEST] Relation reader swap conflict while rebuild inflight...\n");

    auto env = LoadBaselineService();
    auto* service = env.service;

    const char* relation_cfg =
        R"({"name":"client_group_mix","feature_base":"client_group_mix","group_space_id":"client_group","group_space_version":"v1","delta":60,"tz":"Asia/Shanghai","metric_set_id":"net_metrics","metrics":["conn_count"],"encode_type":"exact_sparse","support_policy":{"k_support":8,"min_hist_share":0.005,"min_active_ratio":0.2},"summary_policy":{"k_head":2,"k_stable":2}})";

    IBaselineRelationTask* relation_task = nullptr;
    assert(service->CreateRelationTask(relation_cfg, nullptr, &relation_task) == error::OK);

    BlockingRelationHistoryReader blocking_reader;
    DummyRelationHistoryReader alternate_reader;
    assert(relation_task->SetHistoryReader(&blocking_reader) == error::OK);
    assert(relation_task->RequestRebuild(BaselineStringRef{"svc-relation-conflict", 21},
                                         BaselineRebuildReason::kManual) == error::OK);
    assert(WaitUntil([&]() { return blocking_reader.started(); }));

    assert(relation_task->SetHistoryReader(&alternate_reader) == error::CONFLICT);

    blocking_reader.Allow();
    assert(WaitForTaskCompleted(relation_task));
    assert(relation_task->Close() == error::OK);

    std::printf("[PASS] Relation reader swap conflict while rebuild inflight\n");
}

}  // namespace

int main() {
    TestRebuildWithoutHistoryReader();
    TestRelationReaderSwapConflictWhileRebuildInflight();
    std::printf("[DONE] test_baseline_rebuild\n");
    return 0;
}
