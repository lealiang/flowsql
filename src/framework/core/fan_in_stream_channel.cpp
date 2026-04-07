/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "fan_in_stream_channel.h"

#include <arrow/api.h>

#include <cerrno>
#include <chrono>
#include <thread>
#include <utility>

namespace flowsql {

FanInStreamChannel::FanInStreamChannel(std::string category,
                                       std::string name,
                                       std::vector<std::shared_ptr<IStreamChannel>> sources)
    : category_(std::move(category)),
      name_(std::move(name)),
      sources_(std::move(sources)) {
    source_done_.assign(sources_.size(), false);
}

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

    source_done_.assign(sources_.size(), false);
    rr_cursor_.store(0, std::memory_order_release);
    first_error_.clear();
    cancel_requested_.store(false, std::memory_order_release);
    finished_.store(false, std::memory_order_release);
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

bool FanInStreamChannel::AllSourcesDone() const {
    if (source_done_.empty()) return true;
    for (bool d : source_done_) {
        if (!d) return false;
    }
    return true;
}

bool FanInStreamChannel::PollOneRound(PollEvent* out_event) {
    if (!out_event) return false;
    if (sources_.empty()) return false;

    const size_t n = sources_.size();
    const size_t start = rr_cursor_.fetch_add(1, std::memory_order_acq_rel) % n;
    for (size_t i = 0; i < n; ++i) {
        const size_t idx = (start + i) % n;
        if (source_done_[idx]) continue;
        auto& source = sources_[idx];
        if (!source) {
            source_done_[idx] = true;
            continue;
        }

        PollEvent ev = source->PollNext(0);
        if (ev.kind == PollEventKind::kData) {
            rr_cursor_.store((idx + 1) % n, std::memory_order_release);
            *out_event = std::move(ev);
            return true;
        }
        if (ev.kind == PollEventKind::kTimeout) {
            if (source->IsFinished() && source->IsEmpty()) {
                source_done_[idx] = true;
            }
            continue;
        }
        if (ev.kind == PollEventKind::kEof || ev.kind == PollEventKind::kDrainedAfterCancel) {
            source_done_[idx] = true;
            continue;
        }
        if (ev.kind == PollEventKind::kError) {
            first_error_ = ev.err_msg.empty() ? "fanin source poll error" : ev.err_msg;
            *out_event = PollEvent::Error(ev.err == 0 ? -EIO : ev.err, first_error_);
            return true;
        }
    }
    return false;
}

PollEvent FanInStreamChannel::PollNext(int timeout_ms) {
    if (timeout_ms < 0) timeout_ms = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        PollEvent ev;
        if (PollOneRound(&ev)) {
            if (ev.kind == PollEventKind::kError) {
                finished_.store(true, std::memory_order_release);
            }
            return ev;
        }
        if (!first_error_.empty()) {
            finished_.store(true, std::memory_order_release);
            return PollEvent::Error(-EIO, first_error_);
        }
        if (AllSourcesDone()) {
            finished_.store(true, std::memory_order_release);
            if (cancel_requested_.load(std::memory_order_acquire)) {
                return PollEvent::DrainedAfterCancel();
            }
            return PollEvent::Eof();
        }
        if (timeout_ms == 0) return PollEvent::Timeout();
        if (std::chrono::steady_clock::now() >= deadline) return PollEvent::Timeout();
        std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
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
            first_error_ = "fanin schema mismatch";
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

StreamChannelCapabilities FanInStreamChannel::Capabilities() const {
    StreamChannelCapabilities caps;
    caps.channel_type = ChannelType::kStream;
    caps.concurrency.put_mode = ProducerMode::SINGLE;
    caps.concurrency.poll_mode = ConsumerMode::SINGLE;
    caps.concurrency.max_producers = 1;
    caps.concurrency.max_consumers = 1;
    caps.concurrency.lock_free_put = false;
    caps.concurrency.lock_free_poll = true;
    caps.concurrency.cancel_wakeup_guaranteed = true;
    caps.semantics.finite = IsFinite();
    caps.semantics.supports_timeout_poll = true;
    caps.semantics.supports_filter_pushdown = false;
    caps.semantics.filter_requires_full_match = true;
    caps.semantics.eof_reliable = true;
    caps.semantics.ordering = OrderGuarantee::PER_PRODUCER_FIFO;
    caps.semantics.backpressure = BackpressurePolicy::DROP_OR_BLOCK;
    caps.partition.has_partition_id = false;
    caps.partition.supports_route_by_partition_id = false;
    caps.partition.preserves_partition_order = false;
    return caps;
}

bool FanInStreamChannel::IsFull() const {
    return false;
}

bool FanInStreamChannel::IsEmpty() const {
    for (size_t i = 0; i < sources_.size(); ++i) {
        if (source_done_.size() > i && source_done_[i]) continue;
        if (sources_[i] && !sources_[i]->IsEmpty()) return false;
    }
    return true;
}

size_t FanInStreamChannel::Capacity() const {
    return 0;
}

size_t FanInStreamChannel::Size() const {
    size_t total = 0;
    for (size_t i = 0; i < sources_.size(); ++i) {
        if (source_done_.size() > i && source_done_[i]) continue;
        if (sources_[i]) total += sources_[i]->Size();
    }
    return total;
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

    finished_.store(true, std::memory_order_release);
    opened_.store(false, std::memory_order_release);
}

}  // namespace flowsql
