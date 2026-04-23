/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "rebuild_worker.h"

#include "rebuild_queue.h"

namespace flowsql {
namespace baseline {

RebuildTaskRuntime::RebuildTaskRuntime(std::string task_id, Executor executor)
    : task_id_(std::move(task_id)),
      executor_(std::move(executor)) {}

bool RebuildTaskRuntime::PrepareEnqueue() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_) return false;
    ++pending_count_;
    return true;
}

void RebuildTaskRuntime::OnDequeued(const RebuildRequest&) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_count_ > 0) --pending_count_;
}

void RebuildTaskRuntime::OnCanceled(const RebuildRequest&) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_count_ > 0) --pending_count_;
    }
    cv_.notify_all();
}

bool RebuildTaskRuntime::BeginExecution() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_) return false;
    ++inflight_count_;
    return true;
}

void RebuildTaskRuntime::FinishExecution(const RebuildRequest& request, int status) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (inflight_count_ > 0) --inflight_count_;
        ++completed_count_;
        has_last_result_ = true;
        last_status_ = status;
        last_key_ = request.key;
        last_reason_ = RebuildReasonName(request.rebuild_reason);
    }
    cv_.notify_all();
}

bool RebuildTaskRuntime::CanSwapReader() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !closing_ && pending_count_ == 0 && inflight_count_ == 0;
}

void RebuildTaskRuntime::CloseAndWait() {
    std::unique_lock<std::mutex> lock(mutex_);
    closing_ = true;
    cv_.wait(lock, [this]() {
        return pending_count_ == 0 && inflight_count_ == 0;
    });
}

void RebuildTaskRuntime::Snapshot(RebuildTaskRuntimeSnapshot* out) const {
    if (!out) return;

    std::lock_guard<std::mutex> lock(mutex_);
    out->closing = closing_;
    out->pending_count = pending_count_;
    out->inflight_count = inflight_count_;
    out->completed_count = completed_count_;
    out->has_last_result = has_last_result_;
    out->last_status = last_status_;
    out->last_key = last_key_;
    out->last_reason = last_reason_;
}

int RebuildTaskRuntime::Execute(const RebuildRequest& request) const {
    if (!executor_) return error::UNAVAILABLE;
    return executor_(request);
}

RebuildWorker::RebuildWorker(RebuildQueue* queue)
    : queue_(queue) {}

RebuildWorker::~RebuildWorker() {
    Stop();
}

int RebuildWorker::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return error::OK;
    if (!queue_) return error::UNAVAILABLE;

    running_ = true;
    thread_ = std::thread([this]() { ThreadMain(); });
    return error::OK;
}

void RebuildWorker::Stop() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        running_ = false;
        if (queue_) queue_->Shutdown();
        worker = std::move(thread_);
    }

    if (worker.joinable()) worker.join();
}

bool RebuildWorker::Running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

void RebuildWorker::ThreadMain() {
    RebuildRequest request;
    while (queue_ && queue_->Pop(&request)) {
        auto runtime = request.runtime.lock();
        if (!runtime) continue;
        if (!runtime->BeginExecution()) continue;

        // worker 只持有 task runtime，不直接持有 task 对象本身。
        // 这样即使 task 已从 registry 注销，只要 inflight fetch 尚未完成，
        // 关闭路径也能通过 runtime 的计数安全等待，而不会留下悬空引用。
        const int status = runtime->Execute(request);
        runtime->FinishExecution(request, status);
    }
}

}  // namespace baseline
}  // namespace flowsql
