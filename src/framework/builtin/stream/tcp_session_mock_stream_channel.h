/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_BUILTIN_STREAM_TCP_SESSION_MOCK_STREAM_CHANNEL_H_
#define _FLOWSQL_FRAMEWORK_BUILTIN_STREAM_TCP_SESSION_MOCK_STREAM_CHANNEL_H_

#include <framework/core/ring_stream_channel.h>
#include <framework/interfaces/istream_channel.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace flowsql {

enum class TcpSessionMockMode {
    kNone,
    kStateless,
    kKeyed,
};

struct TcpSessionMockOptions {
    TcpSessionMockMode mode = TcpSessionMockMode::kNone;
    int64_t total_records = 1024;
    int32_t batch_rows = 64;
    int32_t emit_interval_ms = 0;
    int32_t partition_count = 4;
    RingStreamChannelOptions queue_options;
};

class TcpSessionMockStreamChannel : public IStreamChannel {
 public:
    TcpSessionMockStreamChannel(std::string category,
                                std::string name,
                                const TcpSessionMockOptions& options = {});
    ~TcpSessionMockStreamChannel() override;

    // IChannel
    const char* Category() override { return category_.c_str(); }
    const char* Name() override { return name_.c_str(); }
    const char* Type() override { return ChannelType::kStream; }
    const char* Schema() override { return schema_cache_.c_str(); }
    int Open() override;
    int Close() override;
    bool IsOpened() const override { return opened_.load(std::memory_order_acquire); }
    int Flush() override { return 0; }

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
    bool IsFinite() const override { return true; }

    void CloseStream() override;
    void Cancel() override;
    bool IsFinished() const override { return finished_.load(std::memory_order_acquire); }

 private:
    void ProducerLoop();
    std::shared_ptr<arrow::RecordBatch> BuildBatch(int64_t start_index,
                                                   int32_t rows,
                                                   int32_t partition_id) const;
    static int64_t NowMs();
    static std::string MakeClientIp(int32_t client_id);
    static std::string MakeServerIp(int32_t server_id);

    std::string category_;
    std::string name_;
    std::string schema_cache_ = "[]";
    TcpSessionMockOptions options_;
    std::shared_ptr<RingStreamChannel> queue_;
    std::shared_ptr<arrow::Schema> schema_;

    std::atomic<bool> opened_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> finished_{false};
    std::thread producer_thread_;
    mutable std::mutex lifecycle_mu_;
};

TcpSessionMockMode ParseTcpSessionMockMode(const std::string& mode);

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_BUILTIN_STREAM_TCP_SESSION_MOCK_STREAM_CHANNEL_H_
