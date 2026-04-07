/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_CORE_STREAM_HUB_CHANNEL_H_
#define _FLOWSQL_FRAMEWORK_CORE_STREAM_HUB_CHANNEL_H_

#include <framework/core/ring_stream_channel.h>
#include <framework/interfaces/istream_hub_channel.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace flowsql {

enum class StreamHubMode {
    kSplit,
    kMerge,
};

struct StreamHubOptions {
    StreamHubMode mode = StreamHubMode::kSplit;
    int32_t partition_count = 4;
    RingMode partition_ring_mode = RingMode::SPSC;
    size_t partition_ring_size = 256;
};

StreamHubMode ParseStreamHubMode(const std::string& mode);
const char* StreamHubModeName(StreamHubMode mode);

class StreamHubChannel : public IStreamHubChannel {
 public:
    StreamHubChannel(std::string category,
                     std::string name,
                     const StreamHubOptions& options = {});
    ~StreamHubChannel() override;

    StreamHubMode mode() const { return options_.mode; }
    const char* HubMode() const override { return StreamHubModeName(options_.mode); }
    size_t PartitionCount() const override;
    std::shared_ptr<IStreamChannel> GetPartition(size_t idx) const override;
    bool IsHubChannel() const override { return true; }
    const char* HubModeHint() const override { return HubMode(); }
    size_t HubPartitionCount() const override { return PartitionCount(); }
    std::shared_ptr<IStreamChannel> HubPartition(size_t idx) const override {
        return GetPartition(idx);
    }

    // IChannel
    const char* Category() override { return category_.c_str(); }
    const char* Name() override { return name_.c_str(); }
    const char* Type() override { return ChannelType::kStream; }
    const char* Schema() override { return schema_cache_.c_str(); }
    int Open() override;
    int Close() override;
    bool IsOpened() const override { return opened_.load(std::memory_order_acquire); }
    int Flush() override;

    // IStreamChannel
    int Put(std::shared_ptr<arrow::RecordBatch> batch, int64_t ts_ms) override;
    PollEvent PollNext(int timeout_ms = 100) override;
    std::shared_ptr<arrow::Schema> GetOutputSchema() override;
    int SetFilter(const char* condition_json,
                  std::vector<std::string>* unsupported_out) override;
    StreamChannelCapabilities Capabilities() const override;

    bool IsFull() const override;
    bool IsEmpty() const override;
    size_t Capacity() const override;
    size_t Size() const override;
    bool IsFinite() const override { return false; }
    void CloseStream() override;
    void Cancel() override;
    bool IsFinished() const override { return finished_.load(std::memory_order_acquire); }

 private:
    void DispatchLoop();
    size_t ResolveSplitPartition(const StreamBatch& batch);
    void StopDispatchThread();

    std::string category_;
    std::string name_;
    std::string schema_cache_ = "[]";
    StreamHubOptions options_;

    std::shared_ptr<RingStreamChannel> root_;
    std::vector<std::shared_ptr<RingStreamChannel>> partitions_;

    std::atomic<bool> opened_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> finished_{false};
    std::atomic<size_t> rr_cursor_{0};

    mutable std::mutex lifecycle_mu_;
    std::thread dispatch_thread_;
    std::string last_error_;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_STREAM_HUB_CHANNEL_H_
