/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_REBUILD_REBUILD_QUEUE_H_
#define _FLOWSQL_PLUGINS_BASELINE_REBUILD_REBUILD_QUEUE_H_

#include <common/error_code.h>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

#include "rebuild_request.h"

namespace flowsql {
namespace baseline {

class RebuildQueue {
 public:
    int Push(RebuildRequest request);
    bool Pop(RebuildRequest* out);
    size_t CancelTask(const std::string& task_id);
    void Shutdown();
    size_t Size() const;

 private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<RebuildRequest> queue_;
    bool stopped_ = false;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_REBUILD_REBUILD_QUEUE_H_
