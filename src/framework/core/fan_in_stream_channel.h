/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_CORE_FAN_IN_STREAM_CHANNEL_H_
#define _FLOWSQL_FRAMEWORK_CORE_FAN_IN_STREAM_CHANNEL_H_

#include <framework/interfaces/istream_channel.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace flowsql {

class FanInStreamChannel : public IStreamChannel {
 public:
    FanInStreamChannel(std::string category,
                       std::string name,
                       std::vector<std::shared_ptr<IStreamChannel>> sources);
    ~FanInStreamChannel() override;

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
    int SetFilter(const char* condition_json, std::vector<std::string>* unsupported_out) override;
    StreamChannelCapabilities Capabilities() const override;

    bool IsFull() const override;
    bool IsEmpty() const override;
    size_t Capacity() const override;
    size_t Size() const override;
    bool IsFinite() const override;

    void CloseStream() override;
    void Cancel() override;
    bool IsFinished() const override { return finished_.load(std::memory_order_acquire); }

 private:
    bool AllSourcesDone() const;
    bool PollOneRound(PollEvent* out_event);

    std::string category_;
    std::string name_;
    std::string schema_cache_ = "[]";
    std::vector<std::shared_ptr<IStreamChannel>> sources_;
    std::vector<bool> source_done_;
    std::atomic<size_t> rr_cursor_{0};
    std::string first_error_;

    std::atomic<bool> opened_{false};
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> finished_{false};
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_FAN_IN_STREAM_CHANNEL_H_
