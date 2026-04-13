/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_SERVICES_SCHEDULER_SHARED_SOURCE_HUB_H_
#define _FLOWSQL_SERVICES_SCHEDULER_SHARED_SOURCE_HUB_H_

#include <framework/core/ring_stream_channel.h>
#include <framework/interfaces/istream_channel.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace flowsql {
namespace scheduler {

enum class SharedHubMode {
    kDynamic,
    kFixed,
};

enum class SharedHubStatus {
    kCreated,
    kRunning,
    kStopping,
    kStopped,
    kCancelled,
    kFailed,
};

const char* SharedHubModeName(SharedHubMode mode);
const char* SharedHubStatusName(SharedHubStatus status);
bool IsTerminalSharedHubStatus(SharedHubStatus status);

struct SharedSubscriberSnapshot {
    std::string subscriber_id;
    std::string runtime_task_id;
    std::string logical_node_id;
    bool active = false;
    bool ready = false;
    uint64_t delivered_batches = 0;
    uint64_t delivered_rows = 0;
    uint64_t dropped_batches = 0;
    uint64_t dropped_rows = 0;
    uint64_t last_delivered_seq = 0;
    uint64_t last_dropped_seq = 0;
    uint64_t lag = 0;
};

struct SharedHubSnapshot {
    std::string id;
    SharedHubMode mode = SharedHubMode::kDynamic;
    SharedHubStatus status = SharedHubStatus::kCreated;
    std::string source_ref;
    std::vector<std::string> source_keys;
    std::vector<std::string> members;
    std::string where_signature;

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

    std::vector<SharedSubscriberSnapshot> subscribers;
};

struct SharedHubOptions {
    size_t queue_size = 2048;
    int poll_timeout_ms = 50;
    OverflowPolicy overflow_policy = OverflowPolicy::kDrop;
    RingMode ring_mode = RingMode::SPSC;
    bool coordinated_drop = false;
};

class SharedSourceHub;

class SharedSubscriberHandle final {
 public:
    SharedSubscriberHandle() = default;
    ~SharedSubscriberHandle();

    SharedSubscriberHandle(SharedSubscriberHandle&& other) noexcept;
    SharedSubscriberHandle& operator=(SharedSubscriberHandle&& other) noexcept;
    SharedSubscriberHandle(const SharedSubscriberHandle&) = delete;
    SharedSubscriberHandle& operator=(const SharedSubscriberHandle&) = delete;

    bool Valid() const;
    const std::string& SubscriberId() const { return subscriber_id_; }
    const std::string& RuntimeTaskId() const { return runtime_task_id_; }
    std::shared_ptr<IStreamChannel> Input() const { return input_; }

    void Release();

 private:
    friend class SharedSourceHub;

    SharedSubscriberHandle(std::weak_ptr<SharedSourceHub> hub,
                           std::string subscriber_id,
                           std::string runtime_task_id,
                           std::shared_ptr<IStreamChannel> input);

 private:
    std::weak_ptr<SharedSourceHub> hub_;
    std::string subscriber_id_;
    std::string runtime_task_id_;
    std::shared_ptr<IStreamChannel> input_;
    bool released_ = false;
};

class SharedSourceHub final : public std::enable_shared_from_this<SharedSourceHub> {
 public:
    SharedSourceHub(std::string hub_id,
                    SharedHubMode mode,
                    std::string source_ref,
                    std::vector<std::string> source_keys,
                    std::shared_ptr<IStreamChannel> source,
                    SharedHubOptions options = SharedHubOptions{});
    ~SharedSourceHub();

    const std::string& Id() const { return id_; }
    SharedHubMode Mode() const { return mode_; }
    const std::vector<std::string>& SourceKeys() const { return source_keys_; }
    const std::string& SourceRef() const { return source_ref_; }
    size_t SubscriberCount() const;

    bool MatchesWhereSignature(const std::string& where_signature) const;
    int BindWhereSignature(const std::string& where_signature, std::string* err_msg);

    int AddSubscriber(const std::string& runtime_task_id,
                      const std::string& logical_node_id,
                      bool ready,
                      SharedSubscriberHandle* out_handle,
                      std::string* err_msg,
                      size_t max_subscribers = 0,
                      std::shared_ptr<IStreamChannel> input_override = nullptr);
    int MarkSubscriberReady(const std::string& subscriber_id);
    void RemoveSubscriber(const std::string& subscriber_id, bool close_stream = true);

    int Start(std::string* err_msg);
    void RequestStop(bool cancelled = false);
    void Join();

    SharedHubSnapshot Snapshot() const;
    bool IsTerminal() const;

 private:
    struct SubscriberState {
        std::string subscriber_id;
        std::string runtime_task_id;
        std::string logical_node_id;
        std::shared_ptr<IStreamChannel> input;
        bool active = true;
        bool ready = false;
        uint64_t delivered_batches = 0;
        uint64_t delivered_rows = 0;
        uint64_t dropped_batches = 0;
        uint64_t dropped_rows = 0;
        uint64_t last_delivered_seq = 0;
        uint64_t last_dropped_seq = 0;
    };

    std::shared_ptr<IStreamChannel> BuildSubscriberInput(const std::string& subscriber_id,
                                                         int* err_code) const;
    bool AllFixedSubscribersReadyLocked() const;
    void MarkFailedLocked(int code, const std::string& message, int64_t now_ms);
    void RunLoop();

 private:
    const std::string id_;
    const SharedHubMode mode_;
    const std::string source_ref_;
    const std::vector<std::string> source_keys_;
    const SharedHubOptions options_;
    std::shared_ptr<IStreamChannel> source_;

    mutable std::mutex mu_;
    std::thread runner_;
    bool started_ = false;
    std::string where_signature_;

    std::atomic<SharedHubStatus> status_{SharedHubStatus::kCreated};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> cancel_requested_{false};
    uint64_t subscriber_seq_ = 0;
    std::unordered_map<std::string, SubscriberState> subscribers_;

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

#endif  // _FLOWSQL_SERVICES_SCHEDULER_SHARED_SOURCE_HUB_H_
