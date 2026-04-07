/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_SERVICES_SCHEDULER_BROADCAST_HUB_H_
#define _FLOWSQL_SERVICES_SCHEDULER_BROADCAST_HUB_H_

#include <framework/interfaces/istream_channel.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace flowsql {
namespace scheduler {

enum class BroadcastHubStatus {
    kCreated,
    kRunning,
    kStopped,
    kCancelled,
    kFailed,
};

struct BroadcastHubSnapshot {
    std::string id;
    std::string source_ref;
    std::vector<std::string> members;
    BroadcastHubStatus status = BroadcastHubStatus::kCreated;

    uint64_t input_batches = 0;
    uint64_t delivered_batches = 0;
    uint64_t dropped_batches_shared = 0;
    double drop_ratio = 0.0;

    uint64_t input_rows = 0;
    uint64_t delivered_rows = 0;
    uint64_t dropped_rows_shared = 0;

    uint64_t last_delivered_seq = 0;
    uint64_t last_dropped_seq = 0;

    int error_code = 0;
    std::string error_message;
    int64_t started_ms = 0;
    int64_t last_active_ms = 0;
    int64_t finished_ms = 0;
};

const char* BroadcastHubStatusName(BroadcastHubStatus status);
bool IsTerminalBroadcastHubStatus(BroadcastHubStatus status);

class BroadcastHub final {
 public:
    BroadcastHub(std::string id,
                 std::string source_ref,
                 std::vector<std::string> members,
                 std::shared_ptr<IStreamChannel> source,
                 std::vector<std::shared_ptr<IStreamChannel>> member_channels);
    ~BroadcastHub();

    int Start(std::string* err_msg);
    void RequestStop(bool cancelled = false);
    void Join();

    BroadcastHubSnapshot Snapshot() const;
    bool IsTerminal() const;

 private:
    void RunLoop();
    void MarkFailed(int code, const std::string& message, int64_t now_ms);

 private:
    std::string id_;
    std::string source_ref_;
    std::vector<std::string> members_;
    std::shared_ptr<IStreamChannel> source_;
    std::vector<std::shared_ptr<IStreamChannel>> member_channels_;

    mutable std::mutex mu_;
    std::thread runner_;
    bool started_ = false;

    std::atomic<BroadcastHubStatus> status_{BroadcastHubStatus::kCreated};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> cancel_requested_{false};

    uint64_t seq_ = 0;
    uint64_t input_batches_ = 0;
    uint64_t delivered_batches_ = 0;
    uint64_t dropped_batches_shared_ = 0;
    uint64_t input_rows_ = 0;
    uint64_t delivered_rows_ = 0;
    uint64_t dropped_rows_shared_ = 0;
    uint64_t last_delivered_seq_ = 0;
    uint64_t last_dropped_seq_ = 0;

    int error_code_ = 0;
    std::string error_message_;
    int64_t started_ms_ = 0;
    int64_t last_active_ms_ = 0;
    int64_t finished_ms_ = 0;
};

}  // namespace scheduler
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_SCHEDULER_BROADCAST_HUB_H_
