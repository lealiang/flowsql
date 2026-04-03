#include "ring_stream_channel.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <thread>

namespace flowsql {

namespace {

bool IsPowerOfTwo(size_t v) {
    return v > 0 && ((v & (v - 1)) == 0);
}

inline int ToEventError(int rc) {
    if (rc < 0) return rc;
    return -rc;
}

}  // namespace

AtomicRing::AtomicRing(size_t capacity, RingMode mode)
    : slots_(capacity > 0 ? new Slot[capacity] : nullptr),
      capacity_(capacity),
      mask_(capacity > 0 ? (capacity - 1) : 0),
      mode_(mode),
      valid_capacity_(IsPowerOfTwo(capacity)),
      mode_supported_(mode == RingMode::SPSC || mode == RingMode::SPMC) {
    if (slots_) {
        for (size_t i = 0; i < capacity_; ++i) {
            slots_[i].seq.store(i, std::memory_order_relaxed);
        }
    }
}

int AtomicRing::EnqueueSingleProducer(StreamBatch batch) {
    size_t head = head_.load(std::memory_order_relaxed);
    Slot& slot = slots_[head & mask_];
    size_t seq = slot.seq.load(std::memory_order_acquire);
    std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(head);
    if (diff != 0) {
        return EAGAIN;
    }

    slot.batch = std::move(batch);
    slot.seq.store(head + 1, std::memory_order_release);
    head_.store(head + 1, std::memory_order_release);
    return 0;
}

int AtomicRing::DequeueSingleConsumer(StreamBatch* out) {
    if (!out) return EINVAL;

    size_t tail = tail_.load(std::memory_order_relaxed);
    Slot& slot = slots_[tail & mask_];
    size_t seq = slot.seq.load(std::memory_order_acquire);
    std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(tail + 1);
    if (diff != 0) {
        return ETIMEDOUT;
    }

    *out = std::move(slot.batch);
    slot.seq.store(tail + capacity_, std::memory_order_release);
    tail_.store(tail + 1, std::memory_order_release);
    return 0;
}

int AtomicRing::DequeueMultiConsumer(StreamBatch* out) {
    if (!out) return EINVAL;

    while (true) {
        size_t tail = tail_.load(std::memory_order_acquire);
        Slot& slot = slots_[tail & mask_];
        size_t seq = slot.seq.load(std::memory_order_acquire);
        std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(tail + 1);
        if (diff < 0) {
            return ETIMEDOUT;
        }
        if (diff > 0) {
            continue;
        }
        if (tail_.compare_exchange_weak(
                tail, tail + 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            *out = std::move(slot.batch);
            slot.seq.store(tail + capacity_, std::memory_order_release);
            return 0;
        }
    }
}

int AtomicRing::enqueue(StreamBatch batch) {
    if (!mode_supported_) return ENOTSUP;
    if (!valid_capacity_) return EINVAL;
    return EnqueueSingleProducer(std::move(batch));
}

int AtomicRing::dequeue(StreamBatch* out) {
    if (!mode_supported_) return ENOTSUP;
    if (!valid_capacity_) return EINVAL;
    if (mode_ == RingMode::SPSC) {
        return DequeueSingleConsumer(out);
    }
    return DequeueMultiConsumer(out);
}

size_t AtomicRing::size() const {
    size_t head = head_.load(std::memory_order_acquire);
    size_t tail = tail_.load(std::memory_order_acquire);
    return head - tail;
}

size_t AtomicRing::capacity() const {
    return capacity_;
}

RingStreamChannel::RingStreamChannel(std::string category,
                                     std::string name,
                                     const RingStreamChannelOptions& options)
    : category_(std::move(category)),
      name_(std::move(name)),
      options_(options),
      ring_(std::make_unique<AtomicRing>(options.ring_size, options.ring_mode)) {
    if (!options_.finite && options_.overflow == OverflowPolicy::kBlock) {
        options_.overflow = OverflowPolicy::kDrop;
    }

    auto* ring = dynamic_cast<AtomicRing*>(ring_.get());
    if (ring) {
        if (!ring->valid_capacity()) {
            open_error_ = EINVAL;
        } else if (!ring->mode_supported()) {
            open_error_ = ENOTSUP;
        }
    }
}

int RingStreamChannel::Open() {
    if (open_error_ != 0) {
        return open_error_;
    }
    opened_.store(true, std::memory_order_release);
    finished_.store(false, std::memory_order_release);
    cancel_requested_.store(false, std::memory_order_release);
    return 0;
}

int RingStreamChannel::Close() {
    opened_.store(false, std::memory_order_release);
    finished_.store(true, std::memory_order_release);
    cv_.notify_all();
    return 0;
}

int RingStreamChannel::EnqueueWithPolicy(StreamBatch batch, bool block) {
    if (!block) {
        return ring_->enqueue(std::move(batch));
    }

    const StreamBatch retry_payload = batch;
    while (opened_.load(std::memory_order_acquire) &&
           !cancel_requested_.load(std::memory_order_acquire)) {
        int rc = ring_->enqueue(retry_payload);
        if (rc == 0) return 0;
        if (rc != EAGAIN) return rc;

        std::unique_lock<std::mutex> lock(cv_mu_);
        cv_.wait_for(lock, std::chrono::milliseconds(1));
    }
    return ECANCELED;
}

int RingStreamChannel::Put(std::shared_ptr<arrow::RecordBatch> batch, int64_t ts_ms) {
    if (!batch) return EINVAL;
    if (open_error_ != 0) return open_error_;
    if (!opened_.load(std::memory_order_acquire)) return EBADF;
    if (cancel_requested_.load(std::memory_order_acquire)) return ECANCELED;

    StreamBatch payload;
    payload.data = std::move(batch);
    payload.ts_ms = ts_ms;
    payload.partition_id = -1;
    payload.is_eof = false;

    int rc = EnqueueWithPolicy(std::move(payload), options_.overflow == OverflowPolicy::kBlock);
    if (rc == EAGAIN) {
        dropped_batches_.fetch_add(1, std::memory_order_relaxed);
    }
    if (rc == 0) {
        cv_.notify_all();
    }
    return rc;
}

PollEvent RingStreamChannel::PollNext(int timeout_ms) {
    if (open_error_ != 0) {
        return PollEvent::Error(ToEventError(open_error_), "channel not available");
    }

    if (timeout_ms < 0) {
        timeout_ms = 0;
    }

    StreamBatch batch;
    int rc = ring_->dequeue(&batch);
    if (rc == 0) {
        cv_.notify_all();
        return batch.is_eof ? PollEvent::Eof() : PollEvent::Data(std::move(batch));
    }
    if (rc != ETIMEDOUT) {
        return PollEvent::Error(ToEventError(rc), "ring dequeue failed");
    }

    if (cancel_requested_.load(std::memory_order_acquire) &&
        finished_.load(std::memory_order_acquire) &&
        ring_->size() == 0) {
        return PollEvent::DrainedAfterCancel();
    }
    if (finished_.load(std::memory_order_acquire) && ring_->size() == 0) {
        return PollEvent::Eof();
    }

    if (timeout_ms > 0) {
        std::unique_lock<std::mutex> lock(cv_mu_);
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms));
    }

    rc = ring_->dequeue(&batch);
    if (rc == 0) {
        cv_.notify_all();
        return batch.is_eof ? PollEvent::Eof() : PollEvent::Data(std::move(batch));
    }
    if (rc != ETIMEDOUT) {
        return PollEvent::Error(ToEventError(rc), "ring dequeue failed");
    }

    if (cancel_requested_.load(std::memory_order_acquire) &&
        finished_.load(std::memory_order_acquire) &&
        ring_->size() == 0) {
        return PollEvent::DrainedAfterCancel();
    }
    if (finished_.load(std::memory_order_acquire) && ring_->size() == 0) {
        return PollEvent::Eof();
    }
    return PollEvent::Timeout();
}

std::shared_ptr<arrow::Schema> RingStreamChannel::GetOutputSchema() {
    return options_.static_schema;
}

int RingStreamChannel::SetFilter(const char* /* condition_json */,
                                 std::vector<std::string>* unsupported_out) {
    if (unsupported_out) {
        unsupported_out->clear();
    }
    return 0;
}

bool RingStreamChannel::IsFull() const {
    return ring_->size() >= ring_->capacity();
}

bool RingStreamChannel::IsEmpty() const {
    return ring_->size() == 0;
}

size_t RingStreamChannel::Capacity() const {
    return ring_->capacity();
}

size_t RingStreamChannel::Size() const {
    return ring_->size();
}

int RingStreamChannel::PushEofWithRetry(int max_wait_ms) {
    StreamBatch eof;
    eof.is_eof = true;
    const StreamBatch eof_payload = eof;

    int elapsed_ms = 0;
    while (elapsed_ms <= max_wait_ms) {
        int rc = ring_->enqueue(eof_payload);
        if (rc == 0) return 0;
        if (rc != EAGAIN) return rc;

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        elapsed_ms += 10;
    }
    return ETIMEDOUT;
}

void RingStreamChannel::CloseStream() {
    std::call_once(close_stream_once_, [this]() {
        PushEofWithRetry(5000);
        finished_.store(true, std::memory_order_release);
        cv_.notify_all();
    });
}

void RingStreamChannel::Cancel() {
    std::call_once(cancel_once_, [this]() {
        cancel_requested_.store(true, std::memory_order_release);
        PushEofWithRetry(5000);
        finished_.store(true, std::memory_order_release);
        cv_.notify_all();
    });
}

RingMode ParseRingMode(const std::string& mode) {
    if (mode == "spsc") return RingMode::SPSC;
    if (mode == "mpsc") return RingMode::MPSC;
    if (mode == "spmc") return RingMode::SPMC;
    if (mode == "mpmc") return RingMode::MPMC;
    return RingMode::SPSC;
}

OverflowPolicy ParseOverflowPolicy(const std::string& value) {
    return value == "block" ? OverflowPolicy::kBlock : OverflowPolicy::kDrop;
}

}  // namespace flowsql
