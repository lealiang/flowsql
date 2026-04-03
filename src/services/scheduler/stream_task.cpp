#include "stream_task.h"

#include "stream_runtime.h"

#include <arrow/api.h>
#include <common/error_code.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <utility>

namespace flowsql {
namespace scheduler {

namespace {

int64_t CurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

uint64_t EstimateBatchBytes(const arrow::RecordBatch& batch) {
    uint64_t total = 0;
    for (int i = 0; i < batch.num_columns(); ++i) {
        auto arr = batch.column(i);
        if (!arr || !arr->data()) continue;
        for (const auto& buf : arr->data()->buffers) {
            if (buf) total += static_cast<uint64_t>(buf->size());
        }
    }
    return total;
}

void UpdatePeak(std::atomic<uint64_t>* peak, uint64_t v) {
    if (!peak) return;
    uint64_t old = peak->load(std::memory_order_acquire);
    while (v > old && !peak->compare_exchange_weak(
        old, v, std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
}

}  // namespace

ShardRunner::ShardRunner(uint32_t shard_id,
                         std::shared_ptr<IStreamChannel> input,
                         std::shared_ptr<IStreamOperator> op,
                         std::shared_ptr<IChannel> output,
                         StreamTask* owner,
                         bool schema_ready)
    : shard_id_(shard_id),
      input_(std::move(input)),
      op_(std::move(op)),
      output_(std::move(output)),
      owner_(owner),
      schema_ready_(schema_ready) {}

int ShardRunner::Step() {
    constexpr int kBatchBudget = 8;
    int handled = 0;

    const int64_t now_ms = CurrentTimeMs();
    if (started_ms_.load(std::memory_order_relaxed) == 0) {
        started_ms_.store(now_ms, std::memory_order_relaxed);
    }
    last_active_ms_.store(now_ms, std::memory_order_relaxed);
    if (owner_) owner_->TouchActive(now_ms);

    while (handled < kBatchBudget) {
        PollEvent ev = input_->PollNext(0);
        switch (ev.kind) {
            case PollEventKind::kTimeout:
                metrics.poll_timeouts.fetch_add(1, std::memory_order_relaxed);
                if (op_) {
                    int rc = op_->Tick(CurrentTimeMs());
                    if (rc < 0) {
                        if (owner_) owner_->SetFailedOnce(rc, op_->LastError());
                        Finalize();
                        return kStepDone;
                    }
                }
                return kStepNeedRetryLater;

            case PollEventKind::kError:
                metrics.poll_errors.fetch_add(1, std::memory_order_relaxed);
                if (owner_) owner_->SetFailedOnce(ev.err, ev.err_msg.empty() ? "PollNext failed" : ev.err_msg);
                Finalize();
                return kStepDone;

            case PollEventKind::kDrainedAfterCancel:
            case PollEventKind::kEof:
                Finalize();
                return kStepDone;

            case PollEventKind::kData: {
                if (!ev.batch.data) {
                    if (owner_) owner_->SetFailedOnce(EINVAL, "PollEvent::kData without RecordBatch");
                    Finalize();
                    return kStepDone;
                }
                if (!schema_ready_) {
                    int rc = op_->OnSchemaReady(ev.batch.data->schema());
                    if (rc != 0) {
                        if (owner_) owner_->SetFailedOnce(rc, "OnSchemaReady failed");
                        Finalize();
                        return kStepDone;
                    }
                    schema_ready_ = true;
                }

                const uint64_t rows = static_cast<uint64_t>(ev.batch.data->num_rows());
                metrics.processed_batches.fetch_add(1, std::memory_order_relaxed);
                metrics.processed_rows.fetch_add(rows, std::memory_order_relaxed);
                metrics.processed_bytes.fetch_add(EstimateBatchBytes(*ev.batch.data), std::memory_order_relaxed);

                const int rc = op_->Process(*ev.batch.data, ev.batch.ts_ms);
                if (rc == 1) {
                    Finalize();
                    return kStepDone;
                }
                if (rc < 0) {
                    if (owner_) owner_->SetFailedOnce(rc, op_->LastError());
                    Finalize();
                    return kStepDone;
                }
                handled++;
                UpdatePeak(&metrics.queue_depth_peak, static_cast<uint64_t>(QueueDepth()));
                break;
            }
        }
    }
    return kStepYield;
}

int ShardRunner::Finalize() {
    bool expected = false;
    if (!finalized_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return 0;
    }

    if (op_) {
        int rc = op_->Flush();
        if (rc != 0 && owner_) owner_->SetFailedOnce(rc, "Flush failed");
    }

    if (input_) {
        int rc = input_->Close();
        if (rc != 0 && owner_) owner_->SetFailedOnce(rc, "Input close failed");
    }

    finished_ms_.store(CurrentTimeMs(), std::memory_order_relaxed);
    return 0;
}

void ShardRunner::RequestStop() {
    stop_requested.store(true, std::memory_order_relaxed);
    std::call_once(stop_once, [this]() {
        if (input_) input_->Cancel();
    });
}

void ShardRunner::MarkDone() {
    if (owner_) owner_->OnShardDone();
}

size_t ShardRunner::QueueDepth() const {
    if (!input_) return 0;
    return input_->Size();
}

std::string ShardRunner::OpStatsJson() const {
    return op_ ? op_->GetStats() : "{}";
}

StreamTask::StreamTask(std::string task_id, StreamRuntime* runtime)
    : task_id_(std::move(task_id)), runtime_(runtime) {}

void StreamTask::PrepareForRun(uint32_t shard_count, int64_t start_ms) {
    active_shards_.store(shard_count, std::memory_order_release);
    started_ms_.store(start_ms, std::memory_order_release);
    last_active_ms_.store(start_ms, std::memory_order_release);
    status_.store(StreamTaskStatus::kRunning, std::memory_order_release);
}

void StreamTask::AddShard(const std::shared_ptr<ShardRunner>& shard) {
    shards_.push_back(shard);
}

void StreamTask::SetFailedOnce(int code, const std::string& msg) {
    auto expected = std::atomic_load_explicit(&error_, std::memory_order_acquire);
    if (!expected) {
        auto failure = std::make_shared<const ErrorInfo>(ErrorInfo{code, msg});
        std::atomic_compare_exchange_strong_explicit(
            &error_, &expected, failure,
            std::memory_order_release,
            std::memory_order_acquire);
    }
    status_.store(StreamTaskStatus::kFailed, std::memory_order_release);
}

void StreamTask::TouchActive(int64_t now_ms) {
    last_active_ms_.store(now_ms, std::memory_order_relaxed);
}

void StreamTask::OnShardDone() {
    uint32_t prev = active_shards_.load(std::memory_order_acquire);
    while (true) {
        if (prev == 0) return;  // 防御重复回调
        if (active_shards_.compare_exchange_weak(
                prev, prev - 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            break;
        }
    }

    const uint32_t remain = prev - 1;
    if (remain != 0) return;

    if (status_.load(std::memory_order_acquire) != StreamTaskStatus::kFailed) {
        status_.store(
            stop_requested_.load(std::memory_order_relaxed)
                ? StreamTaskStatus::kCancelled
                : StreamTaskStatus::kStopped,
            std::memory_order_release);
    }
    finished_ms_.store(CurrentTimeMs(), std::memory_order_relaxed);
    done_cv_.notify_all();
}

void StreamTask::RequestStop() {
    stop_requested_.store(true, std::memory_order_relaxed);
    while (true) {
        auto st = status_.load(std::memory_order_acquire);
        if (st == StreamTaskStatus::kFailed ||
            st == StreamTaskStatus::kStopped ||
            st == StreamTaskStatus::kCancelled ||
            st == StreamTaskStatus::kStopping) {
            break;
        }
        if (status_.compare_exchange_weak(
                st,
                StreamTaskStatus::kStopping,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            break;
        }
    }

    std::call_once(stop_once_, [this]() {
        for (auto& shard : shards_) {
            shard->RequestStop();
            if (runtime_) runtime_->TrySchedule(shard);
        }
    });
}

void StreamTask::Join() {
    std::unique_lock<std::mutex> lock(done_mu_);
    done_cv_.wait(lock, [this]() {
        return active_shards_.load(std::memory_order_acquire) == 0;
    });
    joined_.store(true, std::memory_order_release);
}

TaskSnapshot StreamTask::Snapshot() const {
    TaskSnapshot s;
    s.task_id = task_id_;
    s.status = status_.load(std::memory_order_acquire);
    s.stop_requested = stop_requested_.load(std::memory_order_acquire);
    s.joined = joined_.load(std::memory_order_acquire);
    s.shard_count = static_cast<uint32_t>(shards_.size());
    s.active_shards = active_shards_.load(std::memory_order_acquire);
    s.started_ms = started_ms_.load(std::memory_order_acquire);
    s.last_active_ms = last_active_ms_.load(std::memory_order_acquire);
    s.finished_ms = finished_ms_.load(std::memory_order_acquire);

    const int64_t end_ms = s.finished_ms > 0 ? s.finished_ms : CurrentTimeMs();
    s.uptime_ms = s.started_ms > 0 ? (end_ms - s.started_ms) : 0;

    for (const auto& shard : shards_) {
        s.processed_batches += shard->metrics.processed_batches.load(std::memory_order_relaxed);
        s.processed_rows += shard->metrics.processed_rows.load(std::memory_order_relaxed);
        s.processed_bytes += shard->metrics.processed_bytes.load(std::memory_order_relaxed);
        s.output_rows += shard->metrics.output_rows.load(std::memory_order_relaxed);
        s.output_batches += shard->metrics.output_batches.load(std::memory_order_relaxed);
        s.dropped_batches += shard->metrics.dropped_batches.load(std::memory_order_relaxed);
        s.poll_timeouts += shard->metrics.poll_timeouts.load(std::memory_order_relaxed);
        s.poll_errors += shard->metrics.poll_errors.load(std::memory_order_relaxed);
        s.queue_depth += shard->QueueDepth();
        s.queue_depth_peak = std::max(
            s.queue_depth_peak,
            shard->metrics.queue_depth_peak.load(std::memory_order_relaxed));
    }

    auto err = std::atomic_load_explicit(&error_, std::memory_order_acquire);
    if (err) {
        s.error_code = err->code;
        s.error_message = err->message;
    }

    std::string merged = "[";
    bool first = true;
    for (const auto& shard : shards_) {
        if (!first) merged += ",";
        merged += shard->OpStatsJson();
        first = false;
    }
    merged += "]";
    s.op_stats_json = std::move(merged);
    return s;
}

}  // namespace scheduler
}  // namespace flowsql
