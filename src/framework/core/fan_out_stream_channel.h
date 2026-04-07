/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_CORE_FAN_OUT_STREAM_CHANNEL_H_
#define _FLOWSQL_FRAMEWORK_CORE_FAN_OUT_STREAM_CHANNEL_H_

#include <framework/core/ring_stream_channel.h>
#include <framework/interfaces/istream_channel.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace flowsql {

enum class FanOutMode {
    ROUND_ROBIN,
    ROUTE_BY_PARTITION_ID,
};

class FanOutStreamChannel : public IStreamChannel {
 public:
    FanOutStreamChannel(std::string category,
                        std::string name,
                        std::shared_ptr<IStreamChannel> source,
                        size_t partition_count,
                        FanOutMode mode = FanOutMode::ROUND_ROBIN,
                        std::string partition_spec = "",
                        const RingStreamChannelOptions& partition_options = {});
    ~FanOutStreamChannel() override;

    // 获取第 i 个分区通道（独立 SPSC ring）
    std::shared_ptr<IStreamChannel> GetPartition(size_t index) const;

    // IChannel
    const char* Category() override { return category_.c_str(); }
    const char* Name() override { return name_.c_str(); }
    const char* Type() override { return ChannelType::kStream; }
    const char* Schema() override { return schema_cache_.c_str(); }
    int Open() override;
    int Close() override;
    bool IsOpened() const override { return opened_.load(std::memory_order_acquire); }
    int Flush() override;

    // IStreamChannel（数据输入来自 source，外部不应直接 Put/Poll 本对象）
    int Put(std::shared_ptr<arrow::RecordBatch> batch, int64_t ts_ms) override;
    PollEvent PollNext(int timeout_ms = 100) override;
    std::shared_ptr<arrow::Schema> GetOutputSchema() override;
    int SetFilter(const char* condition_json, std::vector<std::string>* unsupported_out) override;

    bool IsFull() const override;
    bool IsEmpty() const override;
    size_t Capacity() const override;
    size_t Size() const override;
    bool IsFinite() const override;

    void CloseStream() override;
    void Cancel() override;
    bool IsFinished() const override { return finished_.load(std::memory_order_acquire); }

 private:
    void DispatchLoop();
    bool ResolvePartition(const StreamBatch& batch, size_t* out_index, std::string* err_msg);
    bool ResolvePartitionIdFromMetadata(const StreamBatch& batch, int32_t* out_partition_id) const;

    std::string category_;
    std::string name_;
    std::string schema_cache_ = "[]";

    std::shared_ptr<IStreamChannel> source_;
    std::vector<std::shared_ptr<RingStreamChannel>> partitions_;
    FanOutMode mode_;
    std::string partition_spec_;
    std::string last_error_;

    std::atomic<bool> opened_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> finished_{false};
    std::atomic<size_t> rr_cursor_{0};

    mutable std::mutex lifecycle_mu_;
    std::thread dispatch_thread_;
};

FanOutMode ParseFanOutMode(const std::string& mode);

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_FAN_OUT_STREAM_CHANNEL_H_
