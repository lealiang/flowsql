/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "rebuild_queue.h"

#include <iterator>
#include <vector>

#include "rebuild_worker.h"

namespace flowsql {
namespace baseline {

namespace {

void NotifyCanceled(const RebuildRequest& request) {
    if (auto runtime = request.runtime.lock()) runtime->OnCanceled(request);
}

void NotifyDequeued(const RebuildRequest& request) {
    if (auto runtime = request.runtime.lock()) runtime->OnDequeued(request);
}

}  // namespace

int RebuildQueue::Push(RebuildRequest request) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) return error::UNAVAILABLE;
        queue_.push_back(std::move(request));
    }
    cv_.notify_one();
    return error::OK;
}

bool RebuildQueue::Pop(RebuildRequest* out) {
    if (!out) return false;

    RebuildRequest request;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return stopped_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        request = std::move(queue_.front());
        queue_.pop_front();
    }

    NotifyDequeued(request);
    *out = std::move(request);
    return true;
}

size_t RebuildQueue::CancelTask(const std::string& task_id) {
    std::vector<RebuildRequest> removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = queue_.begin();
        while (it != queue_.end()) {
            if (it->task_id == task_id) {
                removed.push_back(std::move(*it));
                it = queue_.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (const auto& request : removed) NotifyCanceled(request);
    return removed.size();
}

void RebuildQueue::Shutdown() {
    std::vector<RebuildRequest> removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) return;
        stopped_ = true;
        removed.assign(std::make_move_iterator(queue_.begin()),
                       std::make_move_iterator(queue_.end()));
        queue_.clear();
    }

    for (const auto& request : removed) NotifyCanceled(request);
    cv_.notify_all();
}

size_t RebuildQueue::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

}  // namespace baseline
}  // namespace flowsql
