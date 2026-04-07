/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "stream_channel_adapter.h"

#include <arrow/io/api.h>
#include <arrow/ipc/api.h>

#include <cerrno>
#include <utility>
#include <vector>

namespace flowsql {

namespace {

std::string BuildDbError(const char* prefix, const std::string& detail) {
    if (detail.empty()) return std::string(prefix);
    return std::string(prefix) + ": " + detail;
}

}  // namespace

std::shared_ptr<StreamChannelAdapter> StreamChannelAdapter::MakeDirect(
    std::string category,
    std::string name,
    std::shared_ptr<IStreamChannel> stream_sink) {
    auto adapter = std::shared_ptr<StreamChannelAdapter>(
        new StreamChannelAdapter(std::move(category), std::move(name), Mode::kDirectStream));
    adapter->stream_sink_ = std::move(stream_sink);
    return adapter;
}

std::shared_ptr<StreamChannelAdapter> StreamChannelAdapter::MakeDataFrameAppend(
    std::string category,
    std::string name,
    std::shared_ptr<IAppendableDataFrameChannel> dataframe_sink) {
    auto adapter = std::shared_ptr<StreamChannelAdapter>(
        new StreamChannelAdapter(std::move(category), std::move(name), Mode::kDataFrameAppend));
    adapter->dataframe_sink_ = std::move(dataframe_sink);
    return adapter;
}

std::shared_ptr<StreamChannelAdapter> StreamChannelAdapter::MakeDatabaseWriter(
    std::string category,
    std::string name,
    std::shared_ptr<IDatabaseChannel> database_sink,
    std::string table_name) {
    auto adapter = std::shared_ptr<StreamChannelAdapter>(
        new StreamChannelAdapter(std::move(category), std::move(name), Mode::kDatabaseWriter));
    adapter->database_sink_ = std::move(database_sink);
    adapter->table_name_ = std::move(table_name);
    return adapter;
}

StreamChannelAdapter::StreamChannelAdapter(std::string category,
                                           std::string name,
                                           Mode mode)
    : category_(std::move(category)),
      name_(std::move(name)),
      mode_(mode) {}

int StreamChannelAdapter::Open() {
    if (closed_.load(std::memory_order_acquire)) return EBADF;
    opened_.store(true, std::memory_order_release);
    return 0;
}

int StreamChannelAdapter::Close() {
    opened_.store(false, std::memory_order_release);
    closed_.store(true, std::memory_order_release);
    return 0;
}

int StreamChannelAdapter::Flush() {
    if (mode_ == Mode::kDirectStream) {
        if (!stream_sink_) return EINVAL;
        return stream_sink_->Flush();
    }
    // DataFrame append 与 Database writer 当前实现为即时落盘/落通道。
    return 0;
}

int StreamChannelAdapter::Put(std::shared_ptr<arrow::RecordBatch> batch, int64_t ts_ms) {
    (void)ts_ms;
    if (!batch) return EINVAL;
    if (!opened_.load(std::memory_order_acquire)) return EBADF;
    if (closed_.load(std::memory_order_acquire)) return EBADF;
    if (cancelled_.load(std::memory_order_acquire)) return ECANCELED;

    switch (mode_) {
        case Mode::kDirectStream:
            return PutToDirect(std::move(batch), ts_ms);
        case Mode::kDataFrameAppend:
            return PutToDataFrame(std::move(batch));
        case Mode::kDatabaseWriter:
            return PutToDatabase(std::move(batch));
        default:
            return EINVAL;
    }
}

PollEvent StreamChannelAdapter::PollNext(int) {
    return PollEvent::Error(-ENOTSUP, "StreamChannelAdapter is output-only");
}

std::shared_ptr<arrow::Schema> StreamChannelAdapter::GetOutputSchema() {
    if (mode_ == Mode::kDirectStream && stream_sink_) {
        return stream_sink_->GetOutputSchema();
    }
    return nullptr;
}

int StreamChannelAdapter::SetFilter(const char*,
                                    std::vector<std::string>* unsupported_out) {
    if (unsupported_out) unsupported_out->clear();
    return ENOTSUP;
}

bool StreamChannelAdapter::IsFull() const {
    if (mode_ == Mode::kDirectStream && stream_sink_) {
        return stream_sink_->IsFull();
    }
    return false;
}

bool StreamChannelAdapter::IsEmpty() const {
    if (mode_ == Mode::kDirectStream && stream_sink_) {
        return stream_sink_->IsEmpty();
    }
    return true;
}

size_t StreamChannelAdapter::Capacity() const {
    if (mode_ == Mode::kDirectStream && stream_sink_) {
        return stream_sink_->Capacity();
    }
    return 0;
}

size_t StreamChannelAdapter::Size() const {
    if (mode_ == Mode::kDirectStream && stream_sink_) {
        return stream_sink_->Size();
    }
    return 0;
}

void StreamChannelAdapter::Cancel() {
    cancelled_.store(true, std::memory_order_release);
}

bool StreamChannelAdapter::IsFinished() const {
    return closed_.load(std::memory_order_acquire);
}

int StreamChannelAdapter::PutToDirect(std::shared_ptr<arrow::RecordBatch> batch, int64_t ts_ms) {
    if (!stream_sink_) {
        return SetErrorAndReturn(EINVAL, "direct stream sink is null");
    }
    const int rc = stream_sink_->Put(std::move(batch), ts_ms);
    if (rc != 0) {
        return SetErrorAndReturn(rc, "direct stream sink put failed");
    }
    return 0;
}

int StreamChannelAdapter::PutToDataFrame(std::shared_ptr<arrow::RecordBatch> batch) {
    if (!dataframe_sink_) {
        return SetErrorAndReturn(EINVAL, "dataframe sink is null");
    }
    DataFrame df;
    df.FromArrow(std::move(batch));
    const int rc = dataframe_sink_->Append(&df);
    if (rc != 0) {
        return SetErrorAndReturn(rc, "append dataframe failed");
    }
    return 0;
}

int StreamChannelAdapter::PutToDatabase(std::shared_ptr<arrow::RecordBatch> batch) {
    if (!database_sink_) {
        return SetErrorAndReturn(EINVAL, "database sink is null");
    }
    if (table_name_.empty()) {
        return SetErrorAndReturn(EINVAL, "database sink table is empty");
    }

    // 首选 Arrow 列式直写；失败后退化到行式 IBatchWriter。
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches{batch};
    int rc = database_sink_->WriteArrowBatches(table_name_.c_str(), batches);
    if (rc == 0) {
        return 0;
    }

    IBatchWriter* writer = nullptr;
    if (database_sink_->CreateWriter(table_name_.c_str(), &writer) != 0 || !writer) {
        return SetErrorAndReturn(EIO, BuildDbError(
            "create writer failed", GetDatabaseLastError()));
    }

    auto release_writer = [&writer]() {
        if (!writer) return;
        writer->Close(nullptr);
        writer->Release();
        writer = nullptr;
    };

    auto sink_result = arrow::io::BufferOutputStream::Create();
    if (!sink_result.ok()) {
        release_writer();
        return SetErrorAndReturn(EIO, BuildDbError(
            "create buffer stream failed", sink_result.status().ToString()));
    }
    auto sink = *sink_result;

    auto ipc_result = arrow::ipc::MakeStreamWriter(sink, batch->schema());
    if (!ipc_result.ok()) {
        release_writer();
        return SetErrorAndReturn(EIO, BuildDbError(
            "create ipc writer failed", ipc_result.status().ToString()));
    }
    auto ipc_writer = std::move(*ipc_result);

    auto write_status = ipc_writer->WriteRecordBatch(*batch);
    if (!write_status.ok()) {
        release_writer();
        return SetErrorAndReturn(EIO, BuildDbError(
            "serialize batch failed", write_status.ToString()));
    }
    write_status = ipc_writer->Close();
    if (!write_status.ok()) {
        release_writer();
        return SetErrorAndReturn(EIO, BuildDbError(
            "close ipc writer failed", write_status.ToString()));
    }

    auto finish_result = sink->Finish();
    if (!finish_result.ok()) {
        release_writer();
        return SetErrorAndReturn(EIO, BuildDbError(
            "finish ipc buffer failed", finish_result.status().ToString()));
    }
    const std::shared_ptr<arrow::Buffer> buf = *finish_result;
    if (!buf) {
        release_writer();
        return SetErrorAndReturn(EIO, "ipc buffer is empty");
    }

    rc = writer->Write(buf->data(), static_cast<size_t>(buf->size()));
    if (rc != 0) {
        std::string writer_err = writer->GetLastError() ? writer->GetLastError() : "";
        release_writer();
        return SetErrorAndReturn(EIO, BuildDbError("writer write failed", writer_err));
    }
    rc = writer->Flush();
    if (rc != 0) {
        std::string writer_err = writer->GetLastError() ? writer->GetLastError() : "";
        release_writer();
        return SetErrorAndReturn(EIO, BuildDbError("writer flush failed", writer_err));
    }
    release_writer();
    return 0;
}

int StreamChannelAdapter::SetErrorAndReturn(int rc, const std::string& msg) {
    std::lock_guard<std::mutex> lock(error_mu_);
    last_error_ = msg;
    return rc;
}

std::string StreamChannelAdapter::GetDatabaseLastError() const {
    if (!database_sink_) return "";
    const char* err = database_sink_->GetLastError();
    if (!err) return "";
    return err;
}

}  // namespace flowsql
