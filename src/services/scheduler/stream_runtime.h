/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_SERVICES_SCHEDULER_STREAM_RUNTIME_H_
#define _FLOWSQL_SERVICES_SCHEDULER_STREAM_RUNTIME_H_

#include "stream_task.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace flowsql {
namespace scheduler {

template <typename T>
class BlockingQueue {
 public:
    void Push(const T& item) {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_) return;
        q_.push_back(item);
        cv_.notify_one();
    }

    T Pop() {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this]() { return closed_ || !q_.empty(); });
        if (q_.empty()) return T{};
        T item = std::move(q_.front());
        q_.pop_front();
        return item;
    }

    void Close() {
        std::lock_guard<std::mutex> lock(mu_);
        closed_ = true;
        cv_.notify_all();
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(mu_);
        closed_ = false;
        q_.clear();
    }

 private:
    std::deque<T> q_;
    bool closed_ = false;
    std::mutex mu_;
    std::condition_variable cv_;
};

template <typename T>
class DelayQueue {
 public:
    void PushAfter(const T& item, std::chrono::milliseconds delay) {
        const auto deadline = std::chrono::steady_clock::now() + delay;
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_) return;
        heap_.push(Entry{deadline, seq_++, item});
        cv_.notify_one();
    }

    T WaitAndPopDue() {
        std::unique_lock<std::mutex> lock(mu_);
        while (true) {
            if (closed_ && heap_.empty()) return T{};
            if (heap_.empty()) {
                cv_.wait(lock, [this]() { return closed_ || !heap_.empty(); });
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            const auto top = heap_.top();
            if (top.deadline <= now) {
                T item = top.item;
                heap_.pop();
                return item;
            }
            cv_.wait_until(lock, top.deadline);
        }
    }

    void Close() {
        std::lock_guard<std::mutex> lock(mu_);
        closed_ = true;
        cv_.notify_all();
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(mu_);
        closed_ = false;
        seq_ = 0;
        while (!heap_.empty()) {
            heap_.pop();
        }
    }

 private:
    struct Entry {
        std::chrono::steady_clock::time_point deadline;
        uint64_t seq = 0;
        T item;
    };
    struct EntryLess {
        bool operator()(const Entry& a, const Entry& b) const {
            if (a.deadline != b.deadline) return a.deadline > b.deadline;
            return a.seq > b.seq;
        }
    };

    std::priority_queue<Entry, std::vector<Entry>, EntryLess> heap_;
    uint64_t seq_ = 0;
    bool closed_ = false;
    std::mutex mu_;
    std::condition_variable cv_;
};

class StreamRuntime {
 public:
    StreamRuntime() = default;
    ~StreamRuntime();

    void Start(size_t worker_count);
    void Stop();

    bool TrySchedule(const std::shared_ptr<ShardRunner>& shard);
    void WorkerLoop();
    void TimerLoop();
    void OnTimerFire(const std::shared_ptr<ShardRunner>& shard);

 private:
    std::atomic<bool> stopped_{true};
    BlockingQueue<std::shared_ptr<ShardRunner>> ready_queue_;
    DelayQueue<std::shared_ptr<ShardRunner>> timer_queue_;
    std::vector<std::thread> worker_threads_;
    std::thread timer_thread_;
};

}  // namespace scheduler
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_SCHEDULER_STREAM_RUNTIME_H_
