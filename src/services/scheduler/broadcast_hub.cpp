#include "broadcast_hub.h"

#include <arrow/api.h>

#include <chrono>
#include <cerrno>

namespace flowsql {
namespace scheduler {

namespace {

int64_t CurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

const char* BroadcastHubStatusName(BroadcastHubStatus status) {
    switch (status) {
        case BroadcastHubStatus::kCreated: return "created";
        case BroadcastHubStatus::kRunning: return "running";
        case BroadcastHubStatus::kStopped: return "stopped";
        case BroadcastHubStatus::kCancelled: return "cancelled";
        case BroadcastHubStatus::kFailed: return "failed";
        default: return "unknown";
    }
}

bool IsTerminalBroadcastHubStatus(BroadcastHubStatus status) {
    return status == BroadcastHubStatus::kStopped ||
           status == BroadcastHubStatus::kCancelled ||
           status == BroadcastHubStatus::kFailed;
}

BroadcastHub::BroadcastHub(std::string id,
                           std::string source_ref,
                           std::vector<std::string> members,
                           std::shared_ptr<IStreamChannel> source,
                           std::vector<std::shared_ptr<IStreamChannel>> member_channels)
    : id_(std::move(id)),
      source_ref_(std::move(source_ref)),
      members_(std::move(members)),
      source_(std::move(source)),
      member_channels_(std::move(member_channels)) {}

BroadcastHub::~BroadcastHub() {
    RequestStop();
    Join();
}

int BroadcastHub::Start(std::string* err_msg) {
    std::lock_guard<std::mutex> lock(mu_);
    if (started_) return 0;
    if (!source_) {
        if (err_msg) *err_msg = "broadcast source is null";
        return EINVAL;
    }
    if (member_channels_.empty()) {
        if (err_msg) *err_msg = "broadcast member channels are empty";
        return EINVAL;
    }
    started_ = true;
    started_ms_ = CurrentTimeMs();
    last_active_ms_ = started_ms_;
    status_.store(BroadcastHubStatus::kRunning, std::memory_order_release);
    runner_ = std::thread(&BroadcastHub::RunLoop, this);
    return 0;
}

void BroadcastHub::RequestStop(bool cancelled) {
    stop_requested_.store(true, std::memory_order_release);
    if (cancelled) {
        cancel_requested_.store(true, std::memory_order_release);
    }
}

void BroadcastHub::Join() {
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

BroadcastHubSnapshot BroadcastHub::Snapshot() const {
    BroadcastHubSnapshot s;
    std::lock_guard<std::mutex> lock(mu_);
    s.id = id_;
    s.source_ref = source_ref_;
    s.members = members_;
    s.status = status_.load(std::memory_order_acquire);
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
    return s;
}

bool BroadcastHub::IsTerminal() const {
    return IsTerminalBroadcastHubStatus(status_.load(std::memory_order_acquire));
}

void BroadcastHub::MarkFailed(int code, const std::string& message, int64_t now_ms) {
    status_.store(BroadcastHubStatus::kFailed, std::memory_order_release);
    error_code_ = code;
    error_message_ = message;
    finished_ms_ = now_ms;
    last_active_ms_ = now_ms;
}

void BroadcastHub::RunLoop() {
    if (!source_->IsOpened()) {
        const int open_rc = source_->Open();
        if (open_rc != 0) {
            std::lock_guard<std::mutex> lock(mu_);
            MarkFailed(open_rc, "broadcast source open failed", CurrentTimeMs());
            return;
        }
    }
    for (const auto& ch : member_channels_) {
        if (!ch) continue;
        if (!ch->IsOpened()) {
            (void)ch->Open();
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
            for (const auto& ch : member_channels_) {
                if (ch) ch->CloseStream();
            }
            std::lock_guard<std::mutex> lock(mu_);
            status_.store(cancel_requested_.load(std::memory_order_acquire)
                              ? BroadcastHubStatus::kCancelled
                              : BroadcastHubStatus::kStopped,
                          std::memory_order_release);
            finished_ms_ = now_ms;
            return;
        }

        PollEvent ev = source_->PollNext(50);
        if (ev.kind == PollEventKind::kTimeout) {
            continue;
        }
        if (ev.kind == PollEventKind::kEof) {
            for (const auto& ch : member_channels_) {
                if (ch) ch->CloseStream();
            }
            std::lock_guard<std::mutex> lock(mu_);
            status_.store(BroadcastHubStatus::kStopped, std::memory_order_release);
            finished_ms_ = now_ms;
            return;
        }
        if (ev.kind == PollEventKind::kError) {
            std::lock_guard<std::mutex> lock(mu_);
            MarkFailed(ev.err != 0 ? ev.err : EIO,
                       ev.err_msg.empty() ? "broadcast source poll error" : ev.err_msg,
                       now_ms);
            for (const auto& ch : member_channels_) {
                if (ch) ch->CloseStream();
            }
            return;
        }
        if (ev.kind != PollEventKind::kData || !ev.batch.data) {
            continue;
        }

        const uint64_t rows = static_cast<uint64_t>(ev.batch.data->num_rows());
        bool can_put_all = true;
        for (const auto& ch : member_channels_) {
            if (!ch || ch->IsFull()) {
                can_put_all = false;
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            ++seq_;
            ++input_batches_;
            input_rows_ += rows;
            if (!can_put_all) {
                ++dropped_batches_shared_;
                dropped_rows_shared_ += rows;
                last_dropped_seq_ = seq_;
            }
        }
        if (!can_put_all) {
            continue;
        }

        bool dispatch_ok = true;
        int dispatch_rc = 0;
        for (const auto& ch : member_channels_) {
            if (!ch) {
                dispatch_ok = false;
                dispatch_rc = EINVAL;
                break;
            }
            const int rc = ch->Put(ev.batch.data, ev.batch.ts_ms);
            if (rc != 0) {
                dispatch_ok = false;
                dispatch_rc = rc;
                break;
            }
        }
        if (!dispatch_ok) {
            std::lock_guard<std::mutex> lock(mu_);
            MarkFailed(dispatch_rc != 0 ? dispatch_rc : EIO,
                       "broadcast dispatch failed",
                       now_ms);
            for (const auto& ch : member_channels_) {
                if (ch) ch->CloseStream();
            }
            return;
        }

        std::lock_guard<std::mutex> lock(mu_);
        ++delivered_batches_;
        delivered_rows_ += rows;
        last_delivered_seq_ = seq_;
    }
}

}  // namespace scheduler
}  // namespace flowsql
