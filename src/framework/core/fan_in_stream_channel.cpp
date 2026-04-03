#include "fan_in_stream_channel.h"

#include <arrow/api.h>

#include <cerrno>
#include <chrono>
#include <utility>

namespace flowsql {

FanInStreamChannel::FanInStreamChannel(std::string category,
                                       std::string name,
                                       std::vector<std::shared_ptr<IStreamChannel>> sources)
    : category_(std::move(category)),
      name_(std::move(name)),
      sources_(std::move(sources)) {}

FanInStreamChannel::~FanInStreamChannel() {
    Cancel();
}

int FanInStreamChannel::Open() {
    if (opened_.load(std::memory_order_acquire)) return 0;
    if (sources_.empty()) return EINVAL;

    for (const auto& source : sources_) {
        if (!source) return EINVAL;
        if (!source->IsOpened()) {
            int rc = source->Open();
            if (rc != 0) return rc;
        }
    }

    cancel_requested_.store(false, std::memory_order_release);
    finished_.store(false, std::memory_order_release);
    active_forwarders_.store(static_cast<int>(sources_.size()), std::memory_order_release);

    forward_threads_.clear();
    forward_threads_.reserve(sources_.size());
    for (size_t i = 0; i < sources_.size(); ++i) {
        forward_threads_.emplace_back(&FanInStreamChannel::ForwardLoop, this, i);
    }

    opened_.store(true, std::memory_order_release);
    return 0;
}

int FanInStreamChannel::Close() {
    Cancel();
    for (const auto& source : sources_) {
        if (source) source->Close();
    }
    opened_.store(false, std::memory_order_release);
    return 0;
}

int FanInStreamChannel::Flush() {
    return 0;
}

int FanInStreamChannel::Put(std::shared_ptr<arrow::RecordBatch> /*batch*/, int64_t /*ts_ms*/) {
    return ENOTSUP;
}

PollEvent FanInStreamChannel::PollNext(int timeout_ms) {
    if (timeout_ms < 0) timeout_ms = 0;

    std::unique_lock<std::mutex> lock(queue_mu_);
    if (merged_queue_.empty() && timeout_ms > 0) {
        queue_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this]() {
            return !merged_queue_.empty() ||
                   finished_.load(std::memory_order_acquire) ||
                   !last_error_.empty();
        });
    }

    if (!merged_queue_.empty()) {
        StreamBatch batch = std::move(merged_queue_.front());
        merged_queue_.pop_front();
        return batch.is_eof ? PollEvent::Eof() : PollEvent::Data(std::move(batch));
    }

    if (!last_error_.empty()) {
        return PollEvent::Error(-EIO, last_error_);
    }

    if (cancel_requested_.load(std::memory_order_acquire) &&
        finished_.load(std::memory_order_acquire)) {
        return PollEvent::DrainedAfterCancel();
    }
    if (finished_.load(std::memory_order_acquire)) {
        return PollEvent::Eof();
    }
    return PollEvent::Timeout();
}

std::shared_ptr<arrow::Schema> FanInStreamChannel::GetOutputSchema() {
    std::shared_ptr<arrow::Schema> common;
    for (const auto& source : sources_) {
        if (!source) continue;
        auto schema = source->GetOutputSchema();
        if (!schema) continue;
        if (!common) {
            common = std::move(schema);
            continue;
        }
        if (!common->Equals(*schema)) {
            std::lock_guard<std::mutex> lock(queue_mu_);
            last_error_ = "fanin schema mismatch";
            return nullptr;
        }
    }
    return common;
}

int FanInStreamChannel::SetFilter(const char* condition_json,
                                  std::vector<std::string>* unsupported_out) {
    for (const auto& source : sources_) {
        if (!source) continue;
        int rc = source->SetFilter(condition_json, unsupported_out);
        if (rc != 0) return rc;
    }
    return 0;
}

bool FanInStreamChannel::IsFull() const {
    return false;
}

bool FanInStreamChannel::IsEmpty() const {
    std::lock_guard<std::mutex> lock(queue_mu_);
    return merged_queue_.empty();
}

size_t FanInStreamChannel::Capacity() const {
    return 0;
}

size_t FanInStreamChannel::Size() const {
    std::lock_guard<std::mutex> lock(queue_mu_);
    return merged_queue_.size();
}

bool FanInStreamChannel::IsFinite() const {
    for (const auto& source : sources_) {
        if (source && !source->IsFinite()) return false;
    }
    return true;
}

void FanInStreamChannel::CloseStream() {
    for (const auto& source : sources_) {
        if (source) source->CloseStream();
    }
}

void FanInStreamChannel::Cancel() {
    cancel_requested_.store(true, std::memory_order_release);
    for (const auto& source : sources_) {
        if (source) source->Cancel();
    }
    queue_cv_.notify_all();

    for (auto& th : forward_threads_) {
        if (th.joinable()) th.join();
    }
    forward_threads_.clear();

    finished_.store(true, std::memory_order_release);
    opened_.store(false, std::memory_order_release);
}

void FanInStreamChannel::ForwardLoop(size_t index) {
    auto source = sources_[index];
    if (!source) {
        MarkForwarderDone();
        return;
    }

    while (!cancel_requested_.load(std::memory_order_acquire)) {
        PollEvent ev = source->PollNext(100);
        if (ev.kind == PollEventKind::kData) {
            PushMerged(std::move(ev.batch));
            continue;
        }
        if (ev.kind == PollEventKind::kTimeout) {
            if (source->IsFinished() && source->IsEmpty()) break;
            continue;
        }
        if (ev.kind == PollEventKind::kEof || ev.kind == PollEventKind::kDrainedAfterCancel) {
            break;
        }
        if (ev.kind == PollEventKind::kError) {
            std::lock_guard<std::mutex> lock(queue_mu_);
            if (last_error_.empty()) last_error_ = ev.err_msg;
            queue_cv_.notify_all();
            break;
        }
    }

    MarkForwarderDone();
}

void FanInStreamChannel::PushMerged(StreamBatch batch) {
    std::lock_guard<std::mutex> lock(queue_mu_);
    merged_queue_.push_back(std::move(batch));
    queue_cv_.notify_one();
}

void FanInStreamChannel::MarkForwarderDone() {
    if (active_forwarders_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        StreamBatch eof;
        eof.is_eof = true;
        {
            std::lock_guard<std::mutex> lock(queue_mu_);
            merged_queue_.push_back(std::move(eof));
        }
        finished_.store(true, std::memory_order_release);
        queue_cv_.notify_all();
    }
}

}  // namespace flowsql
