/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_REBUILD_REBUILD_WORKER_H_
#define _FLOWSQL_PLUGINS_BASELINE_REBUILD_REBUILD_WORKER_H_

#include <common/error_code.h>

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "rebuild_request.h"

namespace flowsql {
namespace baseline {

class RebuildQueue;

struct RebuildTaskRuntimeSnapshot {
    bool closing = false;
    size_t pending_count = 0;
    size_t inflight_count = 0;
    size_t completed_count = 0;
    bool has_last_result = false;
    int last_status = error::OK;
    std::string last_key;
    std::string last_reason;
};

class RebuildTaskRuntime : public std::enable_shared_from_this<RebuildTaskRuntime> {
 public:
    using Executor = std::function<int(const RebuildRequest&)>;

    RebuildTaskRuntime(std::string task_id, Executor executor);

    bool PrepareEnqueue();
    void OnDequeued(const RebuildRequest& request);
    void OnCanceled(const RebuildRequest& request);
    bool BeginExecution();
    void FinishExecution(const RebuildRequest& request, int status);

    bool CanSwapReader() const;
    void CloseAndWait();
    void Snapshot(RebuildTaskRuntimeSnapshot* out) const;
    int Execute(const RebuildRequest& request) const;

 private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string task_id_;
    Executor executor_;
    bool closing_ = false;
    size_t pending_count_ = 0;
    size_t inflight_count_ = 0;
    size_t completed_count_ = 0;
    bool has_last_result_ = false;
    int last_status_ = error::OK;
    std::string last_key_;
    std::string last_reason_;
};

class RebuildWorker {
 public:
    explicit RebuildWorker(RebuildQueue* queue);
    ~RebuildWorker();

    int Start();
    void Stop();
    bool Running() const;

 private:
    void ThreadMain();

    mutable std::mutex mutex_;
    RebuildQueue* queue_ = nullptr;
    std::thread thread_;
    bool running_ = false;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_REBUILD_REBUILD_WORKER_H_
