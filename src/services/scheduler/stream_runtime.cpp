#include "stream_runtime.h"

namespace flowsql {
namespace scheduler {

StreamRuntime::~StreamRuntime() {
    Stop();
}

void StreamRuntime::Start(size_t worker_count) {
    Stop();

    if (worker_count == 0) worker_count = 1;
    stopped_.store(false, std::memory_order_release);
    ready_queue_.Reset();
    timer_queue_.Reset();

    worker_threads_.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
        worker_threads_.emplace_back([this]() { WorkerLoop(); });
    }
    timer_thread_ = std::thread([this]() { TimerLoop(); });
}

void StreamRuntime::Stop() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    ready_queue_.Close();
    timer_queue_.Close();

    for (auto& th : worker_threads_) {
        if (th.joinable()) th.join();
    }
    worker_threads_.clear();

    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }
}

bool StreamRuntime::TrySchedule(const std::shared_ptr<ShardRunner>& s) {
    if (!s) return false;
    while (true) {
        auto st = s->exec_state.load(std::memory_order_acquire);
        if (st == ShardExecState::kDone ||
            st == ShardExecState::kQueued ||
            st == ShardExecState::kRunningPending) {
            return false;
        }
        if (st == ShardExecState::kWaitingRetry) {
            if (s->exec_state.compare_exchange_weak(st, ShardExecState::kQueued)) {
                ready_queue_.Push(s);
                return true;
            }
            continue;
        }
        if (st == ShardExecState::kIdle) {
            if (s->exec_state.compare_exchange_weak(st, ShardExecState::kQueued)) {
                ready_queue_.Push(s);
                return true;
            }
            continue;
        }
        // st == kRunning
        if (s->exec_state.compare_exchange_weak(st, ShardExecState::kRunningPending)) {
            return true;
        }
    }
}

void StreamRuntime::WorkerLoop() {
    while (!stopped_.load(std::memory_order_acquire)) {
        auto shard = ready_queue_.Pop();
        if (!shard) continue;

        auto expected = ShardExecState::kQueued;
        if (!shard->exec_state.compare_exchange_strong(expected, ShardExecState::kRunning)) {
            continue;
        }

        const int rc = shard->Step();
        if (rc == kStepDone) {
            shard->exec_state.store(ShardExecState::kDone, std::memory_order_release);
            shard->MarkDone();
            continue;
        }

        auto prev = shard->exec_state.exchange(ShardExecState::kIdle, std::memory_order_acq_rel);
        if (prev == ShardExecState::kRunningPending) {
            TrySchedule(shard);
            continue;
        }
        if (rc == kStepYield) {
            TrySchedule(shard);
            continue;
        }
        if (rc == kStepNeedRetryLater) {
            auto idle = ShardExecState::kIdle;
            if (shard->exec_state.compare_exchange_strong(idle, ShardExecState::kWaitingRetry)) {
                timer_queue_.PushAfter(shard, std::chrono::milliseconds(5));
            }
        }
    }
}

void StreamRuntime::OnTimerFire(const std::shared_ptr<ShardRunner>& shard) {
    if (!shard) return;
    auto waiting = ShardExecState::kWaitingRetry;
    if (shard->exec_state.compare_exchange_strong(waiting, ShardExecState::kIdle)) {
        TrySchedule(shard);
    }
}

void StreamRuntime::TimerLoop() {
    while (!stopped_.load(std::memory_order_acquire)) {
        auto shard = timer_queue_.WaitAndPopDue();
        if (!shard) continue;
        OnTimerFire(shard);
    }
}

}  // namespace scheduler
}  // namespace flowsql
