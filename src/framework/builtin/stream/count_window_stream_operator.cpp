/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "count_window_stream_operator.h"

#include <arrow/api.h>
#include <framework/core/stream_channel_adapter.h>
#include <framework/interfaces/idatabase_channel.h>
#include <framework/interfaces/idataframe_channel.h>

#include <cstdlib>
#include <cstdio>

namespace flowsql {

int CountWindowStreamOperator::Work(IChannel*, IChannel*) {
    last_error_ = "count_window_stream only supports stream runtime path";
    return -1;
}

int CountWindowStreamOperator::Configure(const char* key, const char* value) {
    if (!key || !value) return 0;
    if (std::string(key) == "window_rows") {
        const int64_t v = std::atoll(value);
        if (v > 0) window_rows_ = v;
    } else if (std::string(key) == "flush_interval_ms") {
        const int64_t v = std::atoll(value);
        if (v > 0) flush_interval_ms_ = v;
    }
    return 0;
}

int CountWindowStreamOperator::Init(const char* with_params_json, const StreamSinkContext& sink_ctx) {
    last_error_.clear();
    with_params_json_ = with_params_json ? with_params_json : "";
    output_.reset();

    if (!sink_ctx.sink_channel) {
        last_error_ = "sink channel is null";
        return -1;
    }

    if (sink_ctx.sink_type == ChannelType::kStream) {
        auto* out = dynamic_cast<IStreamChannel*>(sink_ctx.sink_channel);
        if (!out) {
            last_error_ = "stream sink cast failed";
            return -1;
        }
        output_ = std::shared_ptr<IStreamChannel>(out, [](IStreamChannel*) {});
    } else if (sink_ctx.sink_type == ChannelType::kDataFrame) {
        auto* appendable = dynamic_cast<IAppendableDataFrameChannel*>(sink_ctx.sink_channel);
        if (!appendable) {
            last_error_ = "dataframe sink must be appendable";
            return -1;
        }
        output_ = StreamChannelAdapter::MakeDataFrameAppend(
            "stream_adapter",
            sink_ctx.into_raw.empty() ? "dataframe.sink" : sink_ctx.into_raw,
            std::shared_ptr<IAppendableDataFrameChannel>(appendable, [](IAppendableDataFrameChannel*) {}));
    } else if (sink_ctx.sink_type == ChannelType::kDatabase) {
        auto* db = dynamic_cast<IDatabaseChannel*>(sink_ctx.sink_channel);
        if (!db) {
            last_error_ = "database sink cast failed";
            return -1;
        }
        if (sink_ctx.table_name.empty()) {
            last_error_ = "builtin stream operator requires explicit table, use INTO <db_type>.<db_name>.<table>";
            return -1;
        }
        output_ = StreamChannelAdapter::MakeDatabaseWriter(
            "stream_adapter",
            sink_ctx.into_raw.empty() ? "database.sink" : sink_ctx.into_raw,
            std::shared_ptr<IDatabaseChannel>(db, [](IDatabaseChannel*) {}),
            sink_ctx.table_name);
    } else {
        last_error_ = "unsupported sink type: " + sink_ctx.sink_type;
        return -1;
    }
    if (!output_) {
        last_error_ = "create output adapter failed";
        return -1;
    }

    summary_schema_ = arrow::schema({
        arrow::field("window_rows", arrow::int64()),
        arrow::field("total_rows", arrow::int64()),
        arrow::field("ts_ms", arrow::int64()),
    });

    total_rows_.store(0, std::memory_order_release);
    pending_rows_.store(0, std::memory_order_release);
    output_rows_.store(0, std::memory_order_release);
    last_emit_ms_ = 0;
    return 0;
}

int CountWindowStreamOperator::OnSchemaReady(std::shared_ptr<arrow::Schema> /*schema*/) {
    return 0;
}

int CountWindowStreamOperator::Process(const arrow::RecordBatch& batch, int64_t ts_ms) {
    if (!output_) {
        last_error_ = "operator not initialized";
        return -1;
    }
    total_rows_.fetch_add(batch.num_rows(), std::memory_order_relaxed);
    int64_t pending = pending_rows_.fetch_add(batch.num_rows(), std::memory_order_relaxed) + batch.num_rows();
    if (pending >= window_rows_) {
        return EmitSummary(ts_ms);
    }
    return 0;
}

int CountWindowStreamOperator::Tick(int64_t current_ms) {
    if (!output_) return -1;
    if (pending_rows_.load(std::memory_order_relaxed) <= 0) return 0;
    if (last_emit_ms_ == 0 || current_ms - last_emit_ms_ >= flush_interval_ms_) {
        return EmitSummary(current_ms);
    }
    return 0;
}

int CountWindowStreamOperator::Flush() {
    if (!output_) return 0;
    if (pending_rows_.load(std::memory_order_relaxed) > 0) {
        return EmitSummary(last_emit_ms_);
    }
    return 0;
}

std::string CountWindowStreamOperator::GetStats() {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "{\"processed_rows\":%lld,\"output_rows\":%lld,\"pending_rows\":%lld}",
                  static_cast<long long>(total_rows_.load(std::memory_order_relaxed)),
                  static_cast<long long>(output_rows_.load(std::memory_order_relaxed)),
                  static_cast<long long>(pending_rows_.load(std::memory_order_relaxed)));
    return buf;
}

int CountWindowStreamOperator::EmitSummary(int64_t ts_ms) {
    arrow::Int64Builder window_builder;
    arrow::Int64Builder total_builder;
    arrow::Int64Builder ts_builder;

    const int64_t pending = pending_rows_.exchange(0, std::memory_order_acq_rel);
    if (pending <= 0) return 0;
    const int64_t total = total_rows_.load(std::memory_order_relaxed);

    if (!window_builder.Append(pending).ok() ||
        !total_builder.Append(total).ok() ||
        !ts_builder.Append(ts_ms).ok()) {
        last_error_ = "build summary columns failed";
        return -1;
    }

    std::shared_ptr<arrow::Array> arr_window;
    std::shared_ptr<arrow::Array> arr_total;
    std::shared_ptr<arrow::Array> arr_ts;
    if (!window_builder.Finish(&arr_window).ok() ||
        !total_builder.Finish(&arr_total).ok() ||
        !ts_builder.Finish(&arr_ts).ok()) {
        last_error_ = "finalize summary columns failed";
        return -1;
    }

    auto batch = arrow::RecordBatch::Make(summary_schema_, 1, {arr_window, arr_total, arr_ts});
    const int rc = output_->Put(batch, ts_ms);
    if (rc != 0) {
        last_error_ = "stream output put failed";
        return -1;
    }
    output_rows_.fetch_add(1, std::memory_order_relaxed);
    last_emit_ms_ = ts_ms;
    return 0;
}

}  // namespace flowsql
