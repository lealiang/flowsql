/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "stream_hub_channel.h"

#include <algorithm>
#include <cctype>
#include <cerrno>

namespace flowsql {
namespace {

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

RingStreamChannelOptions MakeRootOptions() {
    RingStreamChannelOptions options;
    options.ring_size = 256;
    options.batch_rows = 1024;
    options.overflow = OverflowPolicy::kDrop;
    options.ring_mode = RingMode::MPSC;
    options.finite = false;
    return options;
}

RingStreamChannelOptions MakePartitionOptions(const StreamHubOptions& options) {
    RingStreamChannelOptions partition_options;
    partition_options.ring_size = options.partition_ring_size;
    partition_options.batch_rows = 1024;
    partition_options.overflow = OverflowPolicy::kDrop;
    partition_options.ring_mode = options.partition_ring_mode;
    partition_options.finite = false;
    return partition_options;
}

}  // namespace

StreamHubMode ParseStreamHubMode(const std::string& mode) {
    return ToLowerAscii(mode) == "merge" ? StreamHubMode::kMerge : StreamHubMode::kSplit;
}

const char* StreamHubModeName(StreamHubMode mode) {
    return mode == StreamHubMode::kMerge ? "merge" : "split";
}

StreamHubChannel::StreamHubChannel(std::string category,
                                   std::string name,
                                   const StreamHubOptions& options)
    : category_(std::move(category)),
      name_(std::move(name)),
      options_(options) {
    if (options_.partition_count <= 0) options_.partition_count = 1;
    if (options_.partition_ring_size < 2) options_.partition_ring_size = 256;
    root_ = std::make_shared<RingStreamChannel>("ring", name_ + ".root", MakeRootOptions());
    if (options_.mode == StreamHubMode::kSplit) {
        const RingStreamChannelOptions partition_options = MakePartitionOptions(options_);
        partitions_.reserve(static_cast<size_t>(options_.partition_count));
        for (int32_t i = 0; i < options_.partition_count; ++i) {
            partitions_.push_back(std::make_shared<RingStreamChannel>(
                "ring", name_ + ".p" + std::to_string(i), partition_options));
        }
    }
}

StreamHubChannel::~StreamHubChannel() {
    Cancel();
}

size_t StreamHubChannel::PartitionCount() const {
    return partitions_.size();
}

std::shared_ptr<IStreamChannel> StreamHubChannel::GetPartition(size_t idx) const {
    if (idx >= partitions_.size()) return nullptr;
    return partitions_[idx];
}

int StreamHubChannel::Open() {
    std::lock_guard<std::mutex> lock(lifecycle_mu_);
    if (opened_.load(std::memory_order_acquire)) return 0;
    if (!root_) return EINVAL;

    const bool root_was_opened = root_->IsOpened();
    int rc = 0;
    if (!root_was_opened) {
        rc = root_->Open();
        if (rc != 0) return rc;
    }
    size_t opened_partitions = 0;
    for (size_t i = 0; i < partitions_.size(); ++i) {
        const auto& partition = partitions_[i];
        if (!partition) {
            for (size_t j = 0; j < opened_partitions; ++j) {
                if (partitions_[j]) (void)partitions_[j]->Close();
            }
            if (!root_was_opened) {
                (void)root_->Close();
            }
            return EINVAL;
        }
        rc = partition->Open();
        if (rc != 0) {
            for (size_t j = 0; j < opened_partitions; ++j) {
                if (partitions_[j]) (void)partitions_[j]->Close();
            }
            if (!root_was_opened) {
                (void)root_->Close();
            }
            return rc;
        }
        opened_partitions = i + 1;
    }

    stop_requested_.store(false, std::memory_order_release);
    finished_.store(false, std::memory_order_release);
    rr_cursor_.store(0, std::memory_order_release);
    opened_.store(true, std::memory_order_release);
    if (options_.mode == StreamHubMode::kSplit) {
        dispatch_thread_ = std::thread(&StreamHubChannel::DispatchLoop, this);
    }
    return 0;
}

void StreamHubChannel::StopDispatchThread() {
    if (dispatch_thread_.joinable() &&
        dispatch_thread_.get_id() != std::this_thread::get_id()) {
        dispatch_thread_.join();
    }
}

int StreamHubChannel::Close() {
    Cancel();
    std::lock_guard<std::mutex> lock(lifecycle_mu_);
    if (root_) {
        (void)root_->Close();
    }
    for (const auto& partition : partitions_) {
        if (partition) (void)partition->Close();
    }
    opened_.store(false, std::memory_order_release);
    finished_.store(true, std::memory_order_release);
    return 0;
}

int StreamHubChannel::Flush() {
    if (!root_) return EINVAL;
    int rc = root_->Flush();
    if (rc != 0) return rc;
    for (const auto& partition : partitions_) {
        if (partition) {
            rc = partition->Flush();
            if (rc != 0) return rc;
        }
    }
    return 0;
}

int StreamHubChannel::Put(std::shared_ptr<arrow::RecordBatch> batch, int64_t ts_ms) {
    if (!root_) return EINVAL;
    return root_->Put(std::move(batch), ts_ms);
}

PollEvent StreamHubChannel::PollNext(int timeout_ms) {
    if (!root_) return PollEvent::Error(-EINVAL, "stream hub root is null");
    if (options_.mode == StreamHubMode::kSplit) {
        return PollEvent::Error(-ENOTSUP, "stream_hub(split) root source is not allowed");
    }
    return root_->PollNext(timeout_ms);
}

std::shared_ptr<arrow::Schema> StreamHubChannel::GetOutputSchema() {
    if (!root_) return nullptr;
    return root_->GetOutputSchema();
}

int StreamHubChannel::SetFilter(const char* condition_json,
                                std::vector<std::string>* unsupported_out) {
    if (!root_) return EINVAL;
    return root_->SetFilter(condition_json, unsupported_out);
}

StreamChannelCapabilities StreamHubChannel::Capabilities() const {
    if (!root_) return StreamChannelCapabilities{};
    StreamChannelCapabilities caps = root_->Capabilities();
    caps.channel_type = ChannelType::kStream;
    if (options_.mode == StreamHubMode::kSplit) {
        caps.semantics.supports_timeout_poll = false;
    }
    return caps;
}

bool StreamHubChannel::IsFull() const {
    return root_ && root_->IsFull();
}

bool StreamHubChannel::IsEmpty() const {
    if (!root_) return true;
    if (!root_->IsEmpty()) return false;
    for (const auto& partition : partitions_) {
        if (partition && !partition->IsEmpty()) return false;
    }
    return true;
}

size_t StreamHubChannel::Capacity() const {
    if (!root_) return 0;
    size_t total = root_->Capacity();
    for (const auto& partition : partitions_) {
        if (partition) total += partition->Capacity();
    }
    return total;
}

size_t StreamHubChannel::Size() const {
    if (!root_) return 0;
    size_t total = root_->Size();
    for (const auto& partition : partitions_) {
        if (partition) total += partition->Size();
    }
    return total;
}

void StreamHubChannel::CloseStream() {
    if (!root_) return;
    root_->CloseStream();
}

void StreamHubChannel::Cancel() {
    stop_requested_.store(true, std::memory_order_release);
    if (root_) root_->Cancel();
    StopDispatchThread();
    for (const auto& partition : partitions_) {
        if (partition) partition->Cancel();
    }
    opened_.store(false, std::memory_order_release);
    finished_.store(true, std::memory_order_release);
}

size_t StreamHubChannel::ResolveSplitPartition(const StreamBatch& batch) {
    const size_t n = partitions_.size();
    if (n == 0) return 0;
    if (batch.partition_id >= 0) {
        return static_cast<size_t>(batch.partition_id) % n;
    }
    return rr_cursor_.fetch_add(1, std::memory_order_relaxed) % n;
}

void StreamHubChannel::DispatchLoop() {
    if (options_.mode != StreamHubMode::kSplit) return;
    while (!stop_requested_.load(std::memory_order_acquire)) {
        PollEvent ev = root_->PollNext(100);
        if (ev.kind == PollEventKind::kData) {
            if (partitions_.empty()) continue;
            const size_t idx = ResolveSplitPartition(ev.batch);
            auto& partition = partitions_[idx];
            if (!partition) continue;
            const int rc = partition->Put(ev.batch.data, ev.batch.ts_ms);
            if (rc == EAGAIN || rc == ECANCELED) {
                continue;
            }
            if (rc != 0) {
                stop_requested_.store(true, std::memory_order_release);
                root_->Cancel();
                for (const auto& p : partitions_) {
                    if (p) p->Cancel();
                }
                finished_.store(true, std::memory_order_release);
                opened_.store(false, std::memory_order_release);
                return;
            }
            continue;
        }

        if (ev.kind == PollEventKind::kTimeout) {
            if (root_->IsFinished() && root_->IsEmpty()) {
                for (const auto& p : partitions_) {
                    if (p) p->CloseStream();
                }
                finished_.store(true, std::memory_order_release);
                opened_.store(false, std::memory_order_release);
                return;
            }
            continue;
        }

        if (ev.kind == PollEventKind::kEof ||
            ev.kind == PollEventKind::kDrainedAfterCancel) {
            for (const auto& p : partitions_) {
                if (p) p->CloseStream();
            }
            finished_.store(true, std::memory_order_release);
            opened_.store(false, std::memory_order_release);
            return;
        }

        for (const auto& p : partitions_) {
            if (p) p->Cancel();
        }
        finished_.store(true, std::memory_order_release);
        opened_.store(false, std::memory_order_release);
        return;
    }
}

}  // namespace flowsql
