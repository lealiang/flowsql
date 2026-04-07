/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_CORE_STREAM_CHANNEL_ADAPTER_H_
#define _FLOWSQL_FRAMEWORK_CORE_STREAM_CHANNEL_ADAPTER_H_

#include <framework/core/dataframe.h>
#include <framework/interfaces/idatabase_channel.h>
#include <framework/interfaces/idataframe_channel.h>
#include <framework/interfaces/istream_channel.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace flowsql {

// StreamChannelAdapter
// 将 IStreamChannel 输出统一适配到：
// 1) stream sink 直连
// 2) dataframe append
// 3) database writer
class StreamChannelAdapter : public IStreamChannel {
 public:
    enum class Mode {
        kDirectStream,
        kDataFrameAppend,
        kDatabaseWriter,
    };

    static std::shared_ptr<StreamChannelAdapter> MakeDirect(
        std::string category,
        std::string name,
        std::shared_ptr<IStreamChannel> stream_sink);

    static std::shared_ptr<StreamChannelAdapter> MakeDataFrameAppend(
        std::string category,
        std::string name,
        std::shared_ptr<IAppendableDataFrameChannel> dataframe_sink);

    static std::shared_ptr<StreamChannelAdapter> MakeDatabaseWriter(
        std::string category,
        std::string name,
        std::shared_ptr<IDatabaseChannel> database_sink,
        std::string table_name);

    ~StreamChannelAdapter() override = default;

    Mode mode() const { return mode_; }
    const std::string& table_name() const { return table_name_; }

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

    bool IsFull() const override;
    bool IsEmpty() const override;
    size_t Capacity() const override;
    size_t Size() const override;
    bool IsFinite() const override { return false; }

    void CloseStream() override { (void)Close(); }
    void Cancel() override;
    bool IsFinished() const override;

 private:
    StreamChannelAdapter(std::string category,
                         std::string name,
                         Mode mode);

    int PutToDirect(std::shared_ptr<arrow::RecordBatch> batch, int64_t ts_ms);
    int PutToDataFrame(std::shared_ptr<arrow::RecordBatch> batch);
    int PutToDatabase(std::shared_ptr<arrow::RecordBatch> batch);

    int SetErrorAndReturn(int rc, const std::string& msg);
    std::string GetDatabaseLastError() const;

 private:
    std::string category_;
    std::string name_;
    Mode mode_;
    std::string table_name_;
    std::string schema_cache_ = "[]";

    std::shared_ptr<IStreamChannel> stream_sink_;
    std::shared_ptr<IAppendableDataFrameChannel> dataframe_sink_;
    std::shared_ptr<IDatabaseChannel> database_sink_;

    std::atomic<bool> opened_{true};
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> closed_{false};

    mutable std::mutex error_mu_;
    std::string last_error_;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_STREAM_CHANNEL_ADAPTER_H_
