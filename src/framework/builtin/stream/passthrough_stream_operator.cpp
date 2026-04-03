#include "passthrough_stream_operator.h"

#include <arrow/api.h>
#include <framework/core/stream_channel_adapter.h>
#include <framework/interfaces/idatabase_channel.h>
#include <framework/interfaces/idataframe_channel.h>

#include <cstdlib>
#include <cstdio>

namespace flowsql {

int PassthroughStreamOperator::Work(IChannel*, IChannel*) {
    last_error_ = "passthrough_stream only supports stream runtime path";
    return -1;
}

int PassthroughStreamOperator::Configure(const char* key, const char* value) {
    if (!key || !value) return 0;
    if (std::string(key) == "parallelism") {
        const int p = std::atoi(value);
        if (p > 0) parallelism_ = p;
    }
    return 0;
}

int PassthroughStreamOperator::Init(const char* with_params_json, const StreamSinkContext& sink_ctx) {
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
    processed_rows_.store(0, std::memory_order_release);
    output_rows_.store(0, std::memory_order_release);
    return 0;
}

int PassthroughStreamOperator::OnSchemaReady(std::shared_ptr<arrow::Schema> /*schema*/) {
    return 0;
}

int PassthroughStreamOperator::Process(const arrow::RecordBatch& batch, int64_t ts_ms) {
    if (!output_) {
        last_error_ = "operator not initialized";
        return -1;
    }
    auto out_batch = batch.Slice(0, batch.num_rows());
    const int rc = output_->Put(out_batch, ts_ms);
    if (rc != 0) {
        last_error_ = "stream output put failed";
        return -1;
    }
    processed_rows_.fetch_add(batch.num_rows(), std::memory_order_relaxed);
    output_rows_.fetch_add(batch.num_rows(), std::memory_order_relaxed);
    return 0;
}

int PassthroughStreamOperator::Tick(int64_t /*current_ms*/) {
    return 0;
}

int PassthroughStreamOperator::Flush() {
    return 0;
}

std::string PassthroughStreamOperator::GetStats() {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "{\"processed_rows\":%lld,\"output_rows\":%lld,\"pending_rows\":0}",
                  static_cast<long long>(processed_rows_.load(std::memory_order_relaxed)),
                  static_cast<long long>(output_rows_.load(std::memory_order_relaxed)));
    return buf;
}

}  // namespace flowsql
