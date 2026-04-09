/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "shared_source_hub.h"

#include <arrow/api.h>
#include <framework/core/ring_stream_channel.h>

#include <chrono>
#include <cerrno>
#include <cstddef>
#include <utility>

namespace flowsql {
namespace scheduler {

namespace {

int64_t CurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

size_t NextPowerOfTwo(size_t value) {
    if (value <= 1) return 1;
    size_t v = value - 1;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    if (sizeof(size_t) >= 8) {
        v |= v >> 32;
    }
    return v + 1;
}

std::string TrimAsciiSpace(const std::string& input) {
    size_t first = 0;
    while (first < input.size() &&
           (input[first] == ' ' || input[first] == '\t' ||
            input[first] == '\r' || input[first] == '\n')) {
        ++first;
    }
    if (first >= input.size()) return "";
    size_t last = input.size();
    while (last > first &&
           (input[last - 1] == ' ' || input[last - 1] == '\t' ||
            input[last - 1] == '\r' || input[last - 1] == '\n')) {
        --last;
    }
    return input.substr(first, last - first);
}

}  // namespace

const char* SharedHubModeName(SharedHubMode mode) {
    switch (mode) {
        case SharedHubMode::kDynamic: return "dynamic";
        case SharedHubMode::kFixed: return "fixed";
        default: return "unknown";
    }
}

const char* SharedHubStatusName(SharedHubStatus status) {
    switch (status) {
        case SharedHubStatus::kCreated: return "created";
        case SharedHubStatus::kRunning: return "running";
        case SharedHubStatus::kStopping: return "stopping";
        case SharedHubStatus::kStopped: return "stopped";
        case SharedHubStatus::kCancelled: return "cancelled";
        case SharedHubStatus::kFailed: return "failed";
        default: return "unknown";
    }
}

bool IsTerminalSharedHubStatus(SharedHubStatus status) {
    return status == SharedHubStatus::kStopped ||
           status == SharedHubStatus::kCancelled ||
           status == SharedHubStatus::kFailed;
}

SharedSubscriberHandle::SharedSubscriberHandle(std::weak_ptr<SharedSourceHub> hub,
                                               std::string subscriber_id,
                                               std::string runtime_task_id,
                                               std::shared_ptr<IStreamChannel> input)
    : hub_(std::move(hub)),
      subscriber_id_(std::move(subscriber_id)),
      runtime_task_id_(std::move(runtime_task_id)),
      input_(std::move(input)) {}

SharedSubscriberHandle::~SharedSubscriberHandle() {
    Release();
}

SharedSubscriberHandle::SharedSubscriberHandle(SharedSubscriberHandle&& other) noexcept
    : hub_(std::move(other.hub_)),
      subscriber_id_(std::move(other.subscriber_id_)),
      runtime_task_id_(std::move(other.runtime_task_id_)),
      input_(std::move(other.input_)),
      released_(other.released_) {
    other.released_ = true;
}

SharedSubscriberHandle& SharedSubscriberHandle::operator=(SharedSubscriberHandle&& other) noexcept {
    if (this == &other) return *this;
    Release();
    hub_ = std::move(other.hub_);
    subscriber_id_ = std::move(other.subscriber_id_);
    runtime_task_id_ = std::move(other.runtime_task_id_);
    input_ = std::move(other.input_);
    released_ = other.released_;
    other.released_ = true;
    return *this;
}

bool SharedSubscriberHandle::Valid() const {
    return !released_ && !subscriber_id_.empty();
}

void SharedSubscriberHandle::Release() {
    if (released_) return;
    released_ = true;
    auto hub = hub_.lock();
    if (hub && !subscriber_id_.empty()) {
        hub->RemoveSubscriber(subscriber_id_, true);
    }
    subscriber_id_.clear();
    runtime_task_id_.clear();
    input_.reset();
}

SharedSourceHub::SharedSourceHub(std::string hub_id,
                                 SharedHubMode mode,
                                 std::string source_ref,
                                 std::vector<std::string> source_keys,
                                 std::shared_ptr<IStreamChannel> source,
                                 SharedHubOptions options)
    : id_(std::move(hub_id)),
      mode_(mode),
      source_ref_(std::move(source_ref)),
      source_keys_(std::move(source_keys)),
      options_(std::move(options)),
      source_(std::move(source)) {}

SharedSourceHub::~SharedSourceHub() {
    RequestStop();
    Join();
}

size_t SharedSourceHub::SubscriberCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return subscribers_.size();
}

bool SharedSourceHub::MatchesWhereSignature(const std::string& where_signature) const {
    std::lock_guard<std::mutex> lock(mu_);
    return where_signature_ == TrimAsciiSpace(where_signature);
}

int SharedSourceHub::BindWhereSignature(const std::string& where_signature, std::string* err_msg) {
    const std::string normalized = TrimAsciiSpace(where_signature);

    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!where_signature_.empty() && where_signature_ != normalized) {
            if (err_msg) *err_msg = "shared source WHERE signature mismatch";
            return EINVAL;
        }
        if (where_signature_.empty() && normalized.empty()) {
            return 0;
        }
        if (where_signature_.empty() && !normalized.empty() && started_) {
            if (err_msg) *err_msg = "shared source WHERE signature cannot be set after start";
            return EBUSY;
        }
    }

    if (normalized.empty()) return 0;
    if (!source_) {
        if (err_msg) *err_msg = "shared source channel is null";
        return EINVAL;
    }
    std::vector<std::string> unsupported;
    const int rc = source_->SetFilter(normalized.c_str(), &unsupported);
    if (rc != 0) {
        if (err_msg) *err_msg = "shared source SetFilter failed";
        return rc;
    }
    if (!unsupported.empty()) {
        if (err_msg) *err_msg = "shared source SetFilter unsupported";
        return ENOTSUP;
    }
    std::lock_guard<std::mutex> lock(mu_);
    where_signature_ = normalized;
    return 0;
}

std::shared_ptr<IStreamChannel> SharedSourceHub::BuildSubscriberInput(const std::string& subscriber_id) const {
    RingStreamChannelOptions opts;
    opts.ring_size = NextPowerOfTwo(options_.queue_size == 0 ? 64 : options_.queue_size);
    opts.batch_rows = 1024;
    opts.overflow = options_.overflow_policy;
    opts.ring_mode = options_.ring_mode;
    opts.finite = false;
    auto channel = std::make_shared<RingStreamChannel>("ring", id_ + "." + subscriber_id, opts);
    (void)channel->Open();
    return channel;
}

int SharedSourceHub::AddSubscriber(const std::string& runtime_task_id,
                                   const std::string& logical_node_id,
                                   bool ready,
                                   SharedSubscriberHandle* out_handle,
                                   std::string* err_msg,
                                   std::shared_ptr<IStreamChannel> input_override) {
    if (out_handle) out_handle->Release();
    if (runtime_task_id.empty()) {
        if (err_msg) *err_msg = "runtime_task_id is empty";
        return EINVAL;
    }

    std::shared_ptr<IStreamChannel> input = std::move(input_override);
    std::string subscriber_id;
    bool must_start = false;

    {
        std::lock_guard<std::mutex> lock(mu_);
        if (mode_ == SharedHubMode::kFixed && started_) {
            if (err_msg) *err_msg = "fixed shared hub does not allow late join";
            return EBUSY;
        }
        if (status_.load(std::memory_order_acquire) == SharedHubStatus::kFailed ||
            status_.load(std::memory_order_acquire) == SharedHubStatus::kCancelled ||
            status_.load(std::memory_order_acquire) == SharedHubStatus::kStopped) {
            if (err_msg) *err_msg = "shared hub is terminal";
            return EBUSY;
        }

        subscriber_id = "sub" + std::to_string(++subscriber_seq_);
        if (!input) {
            input = BuildSubscriberInput(subscriber_id);
        }
        SubscriberState state;
        state.subscriber_id = subscriber_id;
        state.runtime_task_id = runtime_task_id;
        state.logical_node_id = logical_node_id;
        state.input = input;
        state.active = true;
        state.ready = ready;
        subscribers_[subscriber_id] = std::move(state);

        if (!started_ && mode_ == SharedHubMode::kDynamic) {
            must_start = true;
        }
    }

    if (out_handle) {
        *out_handle = SharedSubscriberHandle(
            weak_from_this(),
            subscriber_id,
            runtime_task_id,
            input);
    }

    if (must_start) {
        const int rc = Start(err_msg);
        if (rc != 0) {
            RemoveSubscriber(subscriber_id, true);
            if (out_handle) out_handle->Release();
            return rc;
        }
    }
    return 0;
}

int SharedSourceHub::MarkSubscriberReady(const std::string& subscriber_id) {
    if (subscriber_id.empty()) return EINVAL;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = subscribers_.find(subscriber_id);
    if (it == subscribers_.end()) return ENOENT;
    it->second.ready = true;
    return 0;
}

void SharedSourceHub::RemoveSubscriber(const std::string& subscriber_id, bool close_stream) {
    if (subscriber_id.empty()) return;
    std::shared_ptr<IStreamChannel> input;
    bool became_empty = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = subscribers_.find(subscriber_id);
        if (it == subscribers_.end()) return;
        input = it->second.input;
        subscribers_.erase(it);
        became_empty = subscribers_.empty();
    }
    if (close_stream && input) {
        input->CloseStream();
        (void)input->Close();
    }
    if (became_empty) {
        RequestStop();
    }
}

bool SharedSourceHub::AllFixedSubscribersReadyLocked() const {
    if (mode_ != SharedHubMode::kFixed) return true;
    if (subscribers_.empty()) return false;
    for (const auto& kv : subscribers_) {
        if (!kv.second.active) continue;
        if (!kv.second.ready) return false;
    }
    return true;
}

void SharedSourceHub::MarkFailedLocked(int code, const std::string& message, int64_t now_ms) {
    status_.store(SharedHubStatus::kFailed, std::memory_order_release);
    error_code_ = code;
    error_message_ = message;
    finished_ms_ = now_ms;
    last_active_ms_ = now_ms;
}

int SharedSourceHub::Start(std::string* err_msg) {
    std::lock_guard<std::mutex> lock(mu_);
    if (started_) return 0;
    if (!source_) {
        if (err_msg) *err_msg = "shared source channel is null";
        return EINVAL;
    }
    if (subscribers_.empty()) {
        if (err_msg) *err_msg = "shared source hub has no subscribers";
        return EINVAL;
    }
    if (!AllFixedSubscribersReadyLocked()) {
        if (err_msg) *err_msg = "shared source fixed subscribers are not all ready";
        return EAGAIN;
    }
    started_ = true;
    started_ms_ = CurrentTimeMs();
    last_active_ms_ = started_ms_;
    status_.store(SharedHubStatus::kRunning, std::memory_order_release);
    runner_ = std::thread(&SharedSourceHub::RunLoop, this);
    return 0;
}

void SharedSourceHub::RequestStop(bool cancelled) {
    stop_requested_.store(true, std::memory_order_release);
    if (cancelled) {
        cancel_requested_.store(true, std::memory_order_release);
    }
}

void SharedSourceHub::Join() {
    std::thread local_runner;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (runner_.joinable()) {
            local_runner = std::move(runner_);
        }
    }
    if (local_runner.joinable()) {
        local_runner.join();
    }
}

SharedHubSnapshot SharedSourceHub::Snapshot() const {
    SharedHubSnapshot s;
    std::lock_guard<std::mutex> lock(mu_);
    s.id = id_;
    s.mode = mode_;
    s.status = status_.load(std::memory_order_acquire);
    s.source_ref = source_ref_;
    s.source_keys = source_keys_;
    s.where_signature = where_signature_;

    s.input_batches = input_batches_;
    s.delivered_batches = delivered_batches_;
    s.dropped_batches_shared = dropped_batches_shared_;
    s.drop_ratio = input_batches_ == 0
        ? 0.0
        : static_cast<double>(dropped_batches_shared_) / static_cast<double>(input_batches_);
    s.input_rows = input_rows_;
    s.delivered_rows = delivered_rows_;
    s.dropped_rows_shared = dropped_rows_shared_;
    s.last_delivered_seq = last_delivered_seq_;
    s.last_dropped_seq = last_dropped_seq_;
    s.error_code = error_code_;
    s.error_message = error_message_;
    s.started_ms = started_ms_;
    s.last_active_ms = last_active_ms_;
    s.finished_ms = finished_ms_;

    s.subscribers.reserve(subscribers_.size());
    for (const auto& kv : subscribers_) {
        SharedSubscriberSnapshot item;
        item.subscriber_id = kv.second.subscriber_id;
        item.runtime_task_id = kv.second.runtime_task_id;
        item.logical_node_id = kv.second.logical_node_id;
        if (!item.logical_node_id.empty()) {
            s.members.push_back(item.logical_node_id);
        }
        item.active = kv.second.active;
        item.ready = kv.second.ready;
        item.delivered_batches = kv.second.delivered_batches;
        item.delivered_rows = kv.second.delivered_rows;
        item.dropped_batches = kv.second.dropped_batches;
        item.dropped_rows = kv.second.dropped_rows;
        item.last_delivered_seq = kv.second.last_delivered_seq;
        item.last_dropped_seq = kv.second.last_dropped_seq;
        item.lag = kv.second.input ? static_cast<uint64_t>(kv.second.input->Size()) : 0;
        s.subscribers.push_back(std::move(item));
    }
    return s;
}

bool SharedSourceHub::IsTerminal() const {
    return IsTerminalSharedHubStatus(status_.load(std::memory_order_acquire));
}

void SharedSourceHub::RunLoop() {
    if (!source_->IsOpened()) {
        const int open_rc = source_->Open();
        if (open_rc != 0) {
            std::lock_guard<std::mutex> lock(mu_);
            MarkFailedLocked(open_rc, "shared source open failed", CurrentTimeMs());
            return;
        }
    }

    while (true) {
        const int64_t now_ms = CurrentTimeMs();
        {
            std::lock_guard<std::mutex> lock(mu_);
            last_active_ms_ = now_ms;
        }

        if (stop_requested_.load(std::memory_order_acquire)) {
            source_->Cancel();
            std::vector<std::shared_ptr<IStreamChannel>> outputs;
            {
                std::lock_guard<std::mutex> lock(mu_);
                status_.store(SharedHubStatus::kStopping, std::memory_order_release);
                for (const auto& kv : subscribers_) {
                    if (kv.second.input) outputs.push_back(kv.second.input);
                }
                status_.store(cancel_requested_.load(std::memory_order_acquire)
                                  ? SharedHubStatus::kCancelled
                                  : SharedHubStatus::kStopped,
                              std::memory_order_release);
                finished_ms_ = now_ms;
            }
            for (const auto& ch : outputs) {
                ch->CloseStream();
            }
            return;
        }

        PollEvent ev = source_->PollNext(options_.poll_timeout_ms > 0 ? options_.poll_timeout_ms : 50);
        if (ev.kind == PollEventKind::kTimeout) {
            continue;
        }
        if (ev.kind == PollEventKind::kEof || ev.kind == PollEventKind::kDrainedAfterCancel) {
            std::vector<std::shared_ptr<IStreamChannel>> outputs;
            {
                std::lock_guard<std::mutex> lock(mu_);
                for (const auto& kv : subscribers_) {
                    if (kv.second.input) outputs.push_back(kv.second.input);
                }
                status_.store(SharedHubStatus::kStopped, std::memory_order_release);
                finished_ms_ = now_ms;
            }
            for (const auto& ch : outputs) {
                ch->CloseStream();
            }
            return;
        }
        if (ev.kind == PollEventKind::kError) {
            std::vector<std::shared_ptr<IStreamChannel>> outputs;
            {
                std::lock_guard<std::mutex> lock(mu_);
                MarkFailedLocked(ev.err != 0 ? ev.err : EIO,
                                 ev.err_msg.empty() ? "shared source poll error" : ev.err_msg,
                                 now_ms);
                for (const auto& kv : subscribers_) {
                    if (kv.second.input) outputs.push_back(kv.second.input);
                }
            }
            for (const auto& ch : outputs) {
                ch->CloseStream();
            }
            return;
        }
        if (ev.kind != PollEventKind::kData || !ev.batch.data) {
            continue;
        }

        struct Target {
            std::string id;
            std::shared_ptr<IStreamChannel> input;
        };
        std::vector<Target> targets;
        uint64_t seq = 0;
        uint64_t rows = static_cast<uint64_t>(ev.batch.data->num_rows());
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++seq_;
            seq = seq_;
            ++input_batches_;
            input_rows_ += rows;
            for (const auto& kv : subscribers_) {
                if (!kv.second.active || !kv.second.input) continue;
                targets.push_back(Target{kv.first, kv.second.input});
            }
        }
        if (targets.empty()) {
            continue;
        }

        const bool coordinated = options_.coordinated_drop || mode_ == SharedHubMode::kFixed;
        if (coordinated) {
            bool can_put_all = true;
            for (const auto& target : targets) {
                if (!target.input || target.input->IsFull()) {
                    can_put_all = false;
                    break;
                }
            }
            if (!can_put_all) {
                std::lock_guard<std::mutex> lock(mu_);
                ++dropped_batches_shared_;
                dropped_rows_shared_ += rows;
                last_dropped_seq_ = seq;
                for (auto& kv : subscribers_) {
                    if (!kv.second.active) continue;
                    ++kv.second.dropped_batches;
                    kv.second.dropped_rows += rows;
                    kv.second.last_dropped_seq = seq;
                }
                continue;
            }
            for (const auto& target : targets) {
                if (!target.input) continue;
                const int rc = target.input->Put(ev.batch.data, ev.batch.ts_ms);
                if (rc != 0) {
                    std::lock_guard<std::mutex> lock(mu_);
                    MarkFailedLocked(rc != 0 ? rc : EIO, "shared source dispatch failed", now_ms);
                    for (const auto& t : targets) {
                        if (t.input) t.input->CloseStream();
                    }
                    return;
                }
            }
            std::lock_guard<std::mutex> lock(mu_);
            ++delivered_batches_;
            delivered_rows_ += rows;
            last_delivered_seq_ = seq;
            for (auto& kv : subscribers_) {
                if (!kv.second.active) continue;
                ++kv.second.delivered_batches;
                kv.second.delivered_rows += rows;
                kv.second.last_delivered_seq = seq;
            }
            continue;
        }

        bool any_delivered = false;
        for (const auto& target : targets) {
            if (!target.input) continue;
            if (target.input->IsFull()) {
                std::lock_guard<std::mutex> lock(mu_);
                auto it = subscribers_.find(target.id);
                if (it != subscribers_.end()) {
                    ++it->second.dropped_batches;
                    it->second.dropped_rows += rows;
                    it->second.last_dropped_seq = seq;
                }
                ++dropped_batches_shared_;
                dropped_rows_shared_ += rows;
                last_dropped_seq_ = seq;
                continue;
            }
            const int rc = target.input->Put(ev.batch.data, ev.batch.ts_ms);
            if (rc == EAGAIN) {
                std::lock_guard<std::mutex> lock(mu_);
                auto it = subscribers_.find(target.id);
                if (it != subscribers_.end()) {
                    ++it->second.dropped_batches;
                    it->second.dropped_rows += rows;
                    it->second.last_dropped_seq = seq;
                }
                ++dropped_batches_shared_;
                dropped_rows_shared_ += rows;
                last_dropped_seq_ = seq;
                continue;
            }
            if (rc != 0) {
                std::lock_guard<std::mutex> lock(mu_);
                MarkFailedLocked(rc != 0 ? rc : EIO, "shared source dispatch failed", now_ms);
                for (const auto& t : targets) {
                    if (t.input) t.input->CloseStream();
                }
                return;
            }
            any_delivered = true;
            std::lock_guard<std::mutex> lock(mu_);
            auto it = subscribers_.find(target.id);
            if (it != subscribers_.end()) {
                ++it->second.delivered_batches;
                it->second.delivered_rows += rows;
                it->second.last_delivered_seq = seq;
            }
        }
        if (any_delivered) {
            std::lock_guard<std::mutex> lock(mu_);
            ++delivered_batches_;
            delivered_rows_ += rows;
            last_delivered_seq_ = seq;
        }
    }
}

}  // namespace scheduler
}  // namespace flowsql
