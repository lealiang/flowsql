/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "fan_out_stream_channel.h"

#include <arrow/api.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <limits>

namespace flowsql {

namespace {

size_t NextRoundRobin(std::atomic<size_t>* cursor, size_t n) {
    if (!cursor || n == 0) return 0;
    return cursor->fetch_add(1, std::memory_order_relaxed) % n;
}

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

}  // namespace

FanOutStreamChannel::FanOutStreamChannel(std::string category,
                                         std::string name,
                                         std::shared_ptr<IStreamChannel> source,
                                         size_t partition_count,
                                         FanOutMode mode,
                                         std::string partition_spec,
                                         const RingStreamChannelOptions& partition_options)
    : category_(std::move(category)),
      name_(std::move(name)),
      source_(std::move(source)),
      mode_(mode),
      partition_spec_(std::move(partition_spec)) {
    if (partition_count == 0) partition_count = 1;

    RingStreamChannelOptions options = partition_options;
    options.ring_mode = RingMode::SPSC;
    for (size_t i = 0; i < partition_count; ++i) {
        const std::string partition_name = name_ + ".p" + std::to_string(i);
        partitions_.push_back(std::make_shared<RingStreamChannel>("ring", partition_name, options));
    }
}

FanOutStreamChannel::~FanOutStreamChannel() {
    Cancel();
}

std::shared_ptr<IStreamChannel> FanOutStreamChannel::GetPartition(size_t index) const {
    if (index >= partitions_.size()) return nullptr;
    return partitions_[index];
}

int FanOutStreamChannel::Open() {
    std::lock_guard<std::mutex> lock(lifecycle_mu_);
    if (opened_.load(std::memory_order_acquire)) return 0;
    if (!source_) return EINVAL;

    if (!source_->IsOpened()) {
        int rc = source_->Open();
        if (rc != 0) return rc;
    }
    for (const auto& p : partitions_) {
        int rc = p->Open();
        if (rc != 0) return rc;
    }

    stop_requested_.store(false, std::memory_order_release);
    finished_.store(false, std::memory_order_release);
    last_error_.clear();
    opened_.store(true, std::memory_order_release);
    dispatch_thread_ = std::thread(&FanOutStreamChannel::DispatchLoop, this);
    return 0;
}

int FanOutStreamChannel::Close() {
    Cancel();
    std::lock_guard<std::mutex> lock(lifecycle_mu_);
    for (const auto& p : partitions_) {
        p->Close();
    }
    opened_.store(false, std::memory_order_release);
    return 0;
}

int FanOutStreamChannel::Flush() {
    for (const auto& p : partitions_) {
        p->Flush();
    }
    return 0;
}

int FanOutStreamChannel::Put(std::shared_ptr<arrow::RecordBatch> /*batch*/, int64_t /*ts_ms*/) {
    return ENOTSUP;
}

PollEvent FanOutStreamChannel::PollNext(int /*timeout_ms*/) {
    std::string err;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mu_);
        err = last_error_;
    }
    if (!err.empty()) {
        return PollEvent::Error(-EIO, err);
    }
    return PollEvent::Error(-ENOTSUP, "fanout wrapper does not support direct poll");
}

std::shared_ptr<arrow::Schema> FanOutStreamChannel::GetOutputSchema() {
    if (!source_) return nullptr;
    return source_->GetOutputSchema();
}

int FanOutStreamChannel::SetFilter(const char* condition_json,
                                   std::vector<std::string>* unsupported_out) {
    if (!source_) return EINVAL;
    return source_->SetFilter(condition_json, unsupported_out);
}

bool FanOutStreamChannel::IsFull() const {
    if (partitions_.empty()) return false;
    for (const auto& p : partitions_) {
        if (!p->IsFull()) return false;
    }
    return true;
}

bool FanOutStreamChannel::IsEmpty() const {
    for (const auto& p : partitions_) {
        if (!p->IsEmpty()) return false;
    }
    return true;
}

size_t FanOutStreamChannel::Capacity() const {
    size_t total = 0;
    for (const auto& p : partitions_) total += p->Capacity();
    return total;
}

size_t FanOutStreamChannel::Size() const {
    size_t total = 0;
    for (const auto& p : partitions_) total += p->Size();
    return total;
}

bool FanOutStreamChannel::IsFinite() const {
    return source_ ? source_->IsFinite() : true;
}

void FanOutStreamChannel::CloseStream() {
    if (source_) source_->CloseStream();
    for (const auto& p : partitions_) {
        p->CloseStream();
    }
    finished_.store(true, std::memory_order_release);
}

void FanOutStreamChannel::Cancel() {
    {
        std::lock_guard<std::mutex> lock(lifecycle_mu_);
        stop_requested_.store(true, std::memory_order_release);
    }
    if (source_) source_->Cancel();

    if (dispatch_thread_.joinable()) {
        dispatch_thread_.join();
    }

    for (const auto& p : partitions_) {
        p->Cancel();
    }
    finished_.store(true, std::memory_order_release);
    opened_.store(false, std::memory_order_release);
}

void FanOutStreamChannel::DispatchLoop() {
    while (!stop_requested_.load(std::memory_order_acquire)) {
        PollEvent ev = source_->PollNext(100);
        if (ev.kind == PollEventKind::kData) {
            if (partitions_.empty()) continue;

            size_t idx = 0;
            std::string route_err;
            if (!ResolvePartition(ev.batch, &idx, &route_err)) {
                {
                    std::lock_guard<std::mutex> lock(lifecycle_mu_);
                    last_error_ = std::move(route_err);
                }
                stop_requested_.store(true, std::memory_order_release);
                if (source_) source_->Cancel();
                for (const auto& p : partitions_) {
                    p->Cancel();
                }
                finished_.store(true, std::memory_order_release);
                opened_.store(false, std::memory_order_release);
                return;
            }

            auto& partition = partitions_[idx];
            int rc = partition->Put(ev.batch.data, ev.batch.ts_ms);
            if (rc == EAGAIN || rc == ECANCELED) {
                continue;
            }
            if (rc != 0) {
                {
                    std::lock_guard<std::mutex> lock(lifecycle_mu_);
                    last_error_ = "fanout dispatch put failed";
                }
                stop_requested_.store(true, std::memory_order_release);
                if (source_) source_->Cancel();
                for (const auto& p : partitions_) {
                    p->Cancel();
                }
                finished_.store(true, std::memory_order_release);
                opened_.store(false, std::memory_order_release);
                return;
            }
            continue;
        }

        if (ev.kind == PollEventKind::kTimeout) {
            if (source_->IsFinished() && source_->IsEmpty()) {
                for (const auto& p : partitions_) {
                    p->CloseStream();
                }
                finished_.store(true, std::memory_order_release);
                opened_.store(false, std::memory_order_release);
                return;
            }
            continue;
        }

        if (ev.kind == PollEventKind::kEof || ev.kind == PollEventKind::kDrainedAfterCancel) {
            for (const auto& p : partitions_) {
                p->CloseStream();
            }
            finished_.store(true, std::memory_order_release);
            opened_.store(false, std::memory_order_release);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(lifecycle_mu_);
            last_error_ = ev.err_msg.empty() ? "fanout source poll failed" : ev.err_msg;
        }
        for (const auto& p : partitions_) {
            p->Cancel();
        }
        finished_.store(true, std::memory_order_release);
        opened_.store(false, std::memory_order_release);
        return;
    }

    for (const auto& p : partitions_) {
        p->Cancel();
    }
    finished_.store(true, std::memory_order_release);
    opened_.store(false, std::memory_order_release);
}

bool FanOutStreamChannel::ResolvePartition(const StreamBatch& batch,
                                           size_t* out_index,
                                           std::string* err_msg) {
    if (!out_index) return false;
    const size_t n = partitions_.size();
    if (n == 0) {
        if (err_msg) *err_msg = "fanout has zero partitions";
        return false;
    }
    if (mode_ == FanOutMode::ROUND_ROBIN) {
        *out_index = NextRoundRobin(&rr_cursor_, n);
        return true;
    }

    int32_t partition_id = batch.partition_id;
    if (partition_id < 0 && !ResolvePartitionIdFromMetadata(batch, &partition_id)) {
        if (err_msg) *err_msg = "missing partition_id for ROUTE_BY_PARTITION_ID";
        return false;
    }
    if (partition_id < 0 || static_cast<size_t>(partition_id) >= n) {
        if (err_msg) *err_msg = "partition_id out of range";
        return false;
    }

    *out_index = static_cast<size_t>(partition_id);
    return true;
}

bool FanOutStreamChannel::ResolvePartitionIdFromMetadata(const StreamBatch& batch,
                                                         int32_t* out_partition_id) const {
    if (!out_partition_id || !batch.data) return false;
    auto schema = batch.data->schema();
    if (!schema) return false;
    auto meta = schema->metadata();
    if (!meta) return false;

    int key_idx = meta->FindKey("flowsql.partition_id");
    if (key_idx < 0) {
        key_idx = meta->FindKey("partition_id");
    }
    if (key_idx < 0) return false;

    const std::string value = meta->value(key_idx);
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || (end && *end != '\0')) return false;
    if (parsed < std::numeric_limits<int32_t>::min() ||
        parsed > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    *out_partition_id = static_cast<int32_t>(parsed);
    return true;
}

FanOutMode ParseFanOutMode(const std::string& mode) {
    const std::string m = ToLowerAscii(mode);
    if (m == "route_by_partition_id" || m == "partition_id" || m == "keyed") {
        return FanOutMode::ROUTE_BY_PARTITION_ID;
    }
    return FanOutMode::ROUND_ROBIN;
}

}  // namespace flowsql
