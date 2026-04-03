#include "tcp_service_merge_stream_operator.h"

#include <arrow/api.h>
#include <framework/core/stream_channel_adapter.h>
#include <framework/interfaces/idatabase_channel.h>
#include <framework/interfaces/idataframe_channel.h>

#include <cstdio>

namespace flowsql {

int TcpServiceMergeStreamOperator::Work(IChannel*, IChannel*) {
    last_error_ = "tcp_service_merge_stream only supports stream runtime path";
    return -1;
}

int TcpServiceMergeStreamOperator::Configure(const char*, const char*) {
    return 0;
}

int TcpServiceMergeStreamOperator::Init(const char* with_params_json, const StreamSinkContext& sink_ctx) {
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

    output_schema_ = arrow::schema({
        arrow::field("clientIP", arrow::utf8()),
        arrow::field("serverIP", arrow::utf8()),
        arrow::field("serverPort", arrow::int32()),
        arrow::field("bps", arrow::int64()),
        arrow::field("pps", arrow::int64()),
    });

    agg_.clear();
    processed_rows_.store(0, std::memory_order_release);
    output_rows_.store(0, std::memory_order_release);
    client_ip_idx_ = -1;
    server_ip_idx_ = -1;
    server_port_idx_ = -1;
    bps_idx_ = -1;
    pps_idx_ = -1;
    schema_ready_ = false;
    last_ts_ms_ = 0;
    return 0;
}

int TcpServiceMergeStreamOperator::OnSchemaReady(std::shared_ptr<arrow::Schema> schema) {
    if (!schema) {
        last_error_ = "input schema is null";
        return -1;
    }
    client_ip_idx_ = schema->GetFieldIndex("clientIP");
    server_ip_idx_ = schema->GetFieldIndex("serverIP");
    server_port_idx_ = schema->GetFieldIndex("serverPort");
    bps_idx_ = schema->GetFieldIndex("bps");
    pps_idx_ = schema->GetFieldIndex("pps");
    if (client_ip_idx_ < 0 || server_ip_idx_ < 0 ||
        server_port_idx_ < 0 || bps_idx_ < 0 || pps_idx_ < 0) {
        last_error_ = "required fields are missing";
        return -1;
    }
    schema_ready_ = true;
    return 0;
}

int TcpServiceMergeStreamOperator::Process(const arrow::RecordBatch& batch, int64_t ts_ms) {
    if (!output_) {
        last_error_ = "operator not initialized";
        return -1;
    }
    if (!schema_ready_) {
        last_error_ = "schema not ready";
        return -1;
    }

    auto client_ip_arr = std::dynamic_pointer_cast<arrow::StringArray>(batch.column(client_ip_idx_));
    auto server_ip_arr = std::dynamic_pointer_cast<arrow::StringArray>(batch.column(server_ip_idx_));
    auto server_port_arr = std::dynamic_pointer_cast<arrow::Int32Array>(batch.column(server_port_idx_));
    auto bps_arr = std::dynamic_pointer_cast<arrow::Int64Array>(batch.column(bps_idx_));
    auto pps_arr = std::dynamic_pointer_cast<arrow::Int64Array>(batch.column(pps_idx_));
    if (!client_ip_arr || !server_ip_arr || !server_port_arr || !bps_arr || !pps_arr) {
        last_error_ = "unexpected input field type";
        return -1;
    }

    for (int64_t r = 0; r < batch.num_rows(); ++r) {
        if (client_ip_arr->IsNull(r) || server_ip_arr->IsNull(r) ||
            server_port_arr->IsNull(r) || bps_arr->IsNull(r) || pps_arr->IsNull(r)) {
            continue;
        }
        ServiceKey key;
        key.client_ip = client_ip_arr->GetString(r);
        key.server_ip = server_ip_arr->GetString(r);
        key.server_port = server_port_arr->Value(r);

        auto& value = agg_[key];
        value.bps += bps_arr->Value(r);
        value.pps += pps_arr->Value(r);
    }
    processed_rows_.fetch_add(batch.num_rows(), std::memory_order_relaxed);
    last_ts_ms_ = ts_ms;
    return 0;
}

int TcpServiceMergeStreamOperator::Tick(int64_t) {
    return 0;
}

int TcpServiceMergeStreamOperator::Flush() {
    if (!output_) return 0;
    if (agg_.empty()) return 0;

    std::shared_ptr<arrow::RecordBatch> batch;
    int rc = BuildOutputBatch(&batch);
    if (rc != 0) return rc;
    if (!batch) return 0;

    rc = output_->Put(batch, last_ts_ms_);
    if (rc != 0) {
        last_error_ = "stream output put failed";
        return -1;
    }
    output_rows_.fetch_add(batch->num_rows(), std::memory_order_relaxed);
    agg_.clear();
    return 0;
}

std::string TcpServiceMergeStreamOperator::GetStats() {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "{\"processed_rows\":%lld,\"output_rows\":%lld,\"groups\":%zu}",
                  static_cast<long long>(processed_rows_.load(std::memory_order_relaxed)),
                  static_cast<long long>(output_rows_.load(std::memory_order_relaxed)),
                  agg_.size());
    return buf;
}

int TcpServiceMergeStreamOperator::BuildOutputBatch(std::shared_ptr<arrow::RecordBatch>* out) {
    if (!out) return -1;
    arrow::StringBuilder client_ip_builder;
    arrow::StringBuilder server_ip_builder;
    arrow::Int32Builder server_port_builder;
    arrow::Int64Builder bps_builder;
    arrow::Int64Builder pps_builder;

    for (const auto& it : agg_) {
        const auto& key = it.first;
        const auto& value = it.second;
        if (!client_ip_builder.Append(key.client_ip).ok() ||
            !server_ip_builder.Append(key.server_ip).ok() ||
            !server_port_builder.Append(key.server_port).ok() ||
            !bps_builder.Append(value.bps).ok() ||
            !pps_builder.Append(value.pps).ok()) {
            last_error_ = "build output columns failed";
            return -1;
        }
    }

    auto client_ip = client_ip_builder.Finish().ValueOrDie();
    auto server_ip = server_ip_builder.Finish().ValueOrDie();
    auto server_port = server_port_builder.Finish().ValueOrDie();
    auto bps = bps_builder.Finish().ValueOrDie();
    auto pps = pps_builder.Finish().ValueOrDie();

    *out = arrow::RecordBatch::Make(
        output_schema_,
        static_cast<int64_t>(agg_.size()),
        {client_ip, server_ip, server_port, bps, pps});
    return 0;
}

}  // namespace flowsql
