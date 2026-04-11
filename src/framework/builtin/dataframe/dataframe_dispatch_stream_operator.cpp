/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "dataframe_dispatch_stream_operator.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <sstream>
#include <type_traits>

#include <framework/interfaces/idataframe_channel.h>

namespace flowsql {
namespace {

constexpr uint64_t kFnv1a64Offset = 14695981039346656037ull;
constexpr uint64_t kFnv1a64Prime = 1099511628211ull;

}  // namespace

std::string DataframeDispatchStreamOperator::ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

void DataframeDispatchStreamOperator::Fnv1aUpdate(uint64_t* state, const void* data, size_t len) {
    if (!state || !data || len == 0) return;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        *state ^= static_cast<uint64_t>(p[i]);
        *state *= kFnv1a64Prime;
    }
}

uint64_t DataframeDispatchStreamOperator::HashFieldValue(const FieldValue& value) {
    uint64_t h = kFnv1a64Offset;
    std::visit([&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
            Fnv1aUpdate(&h, v.data(), v.size());
        } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
            if (!v.empty()) Fnv1aUpdate(&h, v.data(), v.size());
        } else if constexpr (std::is_same_v<T, bool>) {
            const uint8_t b = v ? 1 : 0;
            Fnv1aUpdate(&h, &b, sizeof(b));
        } else {
            Fnv1aUpdate(&h, &v, sizeof(v));
        }
    }, value);
    return h;
}

int64_t DataframeDispatchStreamOperator::NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void DataframeDispatchStreamOperator::SetError(const char* code, const std::string& message) {
    if (!code) {
        last_error_ = message;
        return;
    }
    last_error_ = "[";
    last_error_ += code;
    last_error_ += "] ";
    last_error_ += message;
}

int DataframeDispatchStreamOperator::Configure(const char* key, const char* value) {
    if (!key || !value) return 0;
    const std::string k = ToLowerAscii(key);
    cfg_.keys.insert(k);
    if (k == "strategy") {
        cfg_.has_strategy = true;
        const std::string v = ToLowerAscii(value);
        if (v == "round_robin") {
            cfg_.strategy = DispatchStrategy::kRoundRobin;
            return 0;
        }
        if (v == "hash") {
            cfg_.strategy = DispatchStrategy::kHash;
            return 0;
        }
        if (v == "range") {
            cfg_.strategy = DispatchStrategy::kRange;
            return 0;
        }
        SetError("DATAFRAME_DISPATCH_INVALID_STRATEGY", "invalid strategy: " + std::string(value));
        return 0;
    }

    if (k == "field_name") {
        cfg_.has_field_name = true;
        cfg_.field_name = value;
        return 0;
    }

    if (k == "range_rows") {
        const std::string v = ToLowerAscii(value);
        if (v == "auto") {
            cfg_.has_range_rows = false;
            cfg_.range_rows = 0;
            return 0;
        }
        char* end = nullptr;
        const long long parsed = std::strtoll(value, &end, 10);
        if (end == value || (end && *end != '\0')) {
            SetError("DATAFRAME_DISPATCH_RANGE_ROWS_INVALID",
                     "range_rows must be integer > 0 or auto");
            return 0;
        }
        cfg_.has_range_rows = true;
        cfg_.range_rows = static_cast<int64_t>(parsed);
        return 0;
    }

    if (k == "emit_batch_rows") {
        char* end = nullptr;
        const long long parsed = std::strtoll(value, &end, 10);
        if (end == value || (end && *end != '\0') || parsed <= 0) {
            SetError("DATAFRAME_DISPATCH_INVALID_PARAMETER", "emit_batch_rows must be integer > 0");
            return 0;
        }
        cfg_.emit_batch_rows = static_cast<int64_t>(parsed);
        return 0;
    }

    invalid_key_ = key;
    return 0;
}

int DataframeDispatchStreamOperator::ResolveSinkPartitions(
    IChannel* out,
    std::vector<std::shared_ptr<IStreamChannel>>* partitions) {
    if (!partitions) return -1;
    partitions->clear();
    auto* sink = dynamic_cast<IStreamChannel*>(out);
    if (!sink) {
        SetError("DATAFRAME_DISPATCH_INVALID_SINK", "sink channel is not stream");
        return -1;
    }
    if (!sink->IsHubChannel()) {
        SetError("DATAFRAME_DISPATCH_INVALID_SINK", "sink must be stream_hub(split)");
        return -1;
    }
    if (ToLowerAscii(sink->HubModeHint() ? sink->HubModeHint() : "") != "split") {
        SetError("DATAFRAME_DISPATCH_INVALID_SINK", "sink must be stream_hub(split)");
        return -1;
    }
    const size_t n = sink->HubPartitionCount();
    if (n == 0) {
        SetError("DATAFRAME_DISPATCH_PARTITION_INVALID", "stream_hub has no partitions");
        return -1;
    }
    partitions->reserve(n);
    for (size_t i = 0; i < n; ++i) {
        auto p = sink->HubPartition(i);
        if (!p) {
            SetError("DATAFRAME_DISPATCH_PARTITION_INVALID",
                     "stream_hub partition resolve failed at index " + std::to_string(i));
            partitions->clear();
            return -1;
        }
        partitions->push_back(std::move(p));
    }
    return 0;
}

int DataframeDispatchStreamOperator::ResolveEffectiveConfig(const DispatchConfig& in,
                                                            EffectiveConfig* out) {
    if (!out) return -1;

    if (!invalid_key_.empty()) {
        SetError("DATAFRAME_DISPATCH_INVALID_PARAMETER", "unsupported WITH parameter: " + invalid_key_);
        return -1;
    }
    if (!last_error_.empty()) return -1;

    bool has_business_param = false;
    for (const auto& key : in.keys) {
        if (key == "strategy" || key == "field_name" || key == "range_rows") {
            has_business_param = true;
            break;
        }
    }

    out->emit_batch_rows = std::max<int64_t>(1, in.emit_batch_rows);
    if (!in.has_strategy) {
        if (!has_business_param) {
            out->strategy = DispatchStrategy::kRange;
            out->range_auto = true;
            out->range_rows = 0;
            return 0;
        }
        if (in.has_field_name && !in.has_range_rows && in.keys.size() == 1) {
            out->strategy = DispatchStrategy::kHash;
            out->field_name = in.field_name;
            out->range_auto = true;
            out->range_rows = 0;
            return 0;
        }
        SetError("DATAFRAME_DISPATCH_STRATEGY_REQUIRED",
                 "strategy is required unless no params or only field_name is provided");
        return -1;
    }

    out->strategy = in.strategy;
    if (in.strategy == DispatchStrategy::kRoundRobin) {
        if (in.has_field_name || in.has_range_rows) {
            SetError("DATAFRAME_DISPATCH_PARAM_CONFLICT",
                     "round_robin does not accept field_name/range_rows");
            return -1;
        }
        out->range_auto = true;
        out->range_rows = 0;
        return 0;
    }

    if (in.strategy == DispatchStrategy::kHash) {
        if (!in.has_field_name || in.field_name.empty()) {
            SetError("DATAFRAME_DISPATCH_FIELD_REQUIRED", "field_name is required for strategy=hash");
            return -1;
        }
        if (in.has_range_rows) {
            SetError("DATAFRAME_DISPATCH_PARAM_CONFLICT", "strategy=hash does not accept range_rows");
            return -1;
        }
        out->field_name = in.field_name;
        out->range_auto = true;
        out->range_rows = 0;
        return 0;
    }

    if (in.has_field_name) {
        SetError("DATAFRAME_DISPATCH_PARAM_CONFLICT", "strategy=range does not accept field_name");
        return -1;
    }
    out->range_auto = !in.has_range_rows;
    out->range_rows = in.has_range_rows ? in.range_rows : 0;
    if (!out->range_auto && out->range_rows <= 0) {
        SetError("DATAFRAME_DISPATCH_RANGE_ROWS_INVALID", "range_rows must be > 0");
        return -1;
    }
    return 0;
}

int DataframeDispatchStreamOperator::FlushPartitionBuffer(DataFrame* buffer,
                                                          const std::vector<Field>& schema,
                                                          IStreamChannel* partition,
                                                          int64_t ts_ms) {
    if (!buffer || !partition) return -1;
    if (buffer->RowCount() == 0) return 0;
    auto batch = buffer->ToArrow();
    if (!batch) return -1;
    const int rc = partition->Put(std::move(batch), ts_ms);
    if (rc != 0) return rc;
    buffer->Clear();
    buffer->SetSchema(schema);
    return 0;
}

int DataframeDispatchStreamOperator::DispatchRoundRobin(
    const DataFrame& data,
    const EffectiveConfig& cfg,
    const std::vector<std::shared_ptr<IStreamChannel>>& partitions) {
    const size_t n = partitions.size();
    std::vector<Field> schema = data.GetSchema();
    std::vector<DataFrame> buffers(n);
    std::vector<int64_t> pending_rows(n, 0);
    for (size_t i = 0; i < n; ++i) {
        buffers[i].SetSchema(schema);
    }
    const int64_t ts = NowMs();
    for (int32_t i = 0; i < data.RowCount(); ++i) {
        const size_t p = static_cast<size_t>(i) % n;
        if (buffers[p].AppendRow(data.GetRow(i)) != 0) return -1;
        pending_rows[p] += 1;
        if (pending_rows[p] >= cfg.emit_batch_rows) {
            const int rc = FlushPartitionBuffer(&buffers[p], schema, partitions[p].get(), ts);
            if (rc != 0) return rc;
            pending_rows[p] = 0;
        }
    }
    for (size_t p = 0; p < n; ++p) {
        if (pending_rows[p] <= 0) continue;
        const int rc = FlushPartitionBuffer(&buffers[p], schema, partitions[p].get(), ts);
        if (rc != 0) return rc;
        pending_rows[p] = 0;
    }
    return 0;
}

int DataframeDispatchStreamOperator::DispatchHash(
    const DataFrame& data,
    const EffectiveConfig& cfg,
    const std::vector<std::shared_ptr<IStreamChannel>>& partitions) {
    const size_t n = partitions.size();
    const auto values = data.GetColumn(cfg.field_name);
    if (values.empty() && data.RowCount() > 0) {
        SetError("DATAFRAME_DISPATCH_FIELD_NOT_FOUND", "field not found: " + cfg.field_name);
        return -1;
    }
    if (static_cast<int32_t>(values.size()) != data.RowCount()) {
        SetError("DATAFRAME_DISPATCH_FIELD_NOT_FOUND", "field not found: " + cfg.field_name);
        return -1;
    }

    std::vector<Field> schema = data.GetSchema();
    std::vector<DataFrame> buffers(n);
    std::vector<int64_t> pending_rows(n, 0);
    for (size_t i = 0; i < n; ++i) {
        buffers[i].SetSchema(schema);
    }
    const int64_t ts = NowMs();
    for (int32_t i = 0; i < data.RowCount(); ++i) {
        const auto& fv = values[static_cast<size_t>(i)];
        const size_t p = static_cast<size_t>(HashFieldValue(fv) % n);
        if (buffers[p].AppendRow(data.GetRow(i)) != 0) return -1;
        pending_rows[p] += 1;
        if (pending_rows[p] >= cfg.emit_batch_rows) {
            const int rc = FlushPartitionBuffer(&buffers[p], schema, partitions[p].get(), ts);
            if (rc != 0) return rc;
            pending_rows[p] = 0;
        }
    }
    for (size_t p = 0; p < n; ++p) {
        if (pending_rows[p] <= 0) continue;
        const int rc = FlushPartitionBuffer(&buffers[p], schema, partitions[p].get(), ts);
        if (rc != 0) return rc;
        pending_rows[p] = 0;
    }
    return 0;
}

int DataframeDispatchStreamOperator::DispatchRange(
    const DataFrame& data,
    const EffectiveConfig& cfg,
    const std::vector<std::shared_ptr<IStreamChannel>>& partitions) {
    const auto batch = data.ToArrow();
    if (!batch && data.RowCount() > 0) return -1;
    if (!batch || data.RowCount() == 0) return 0;

    const size_t n = partitions.size();
    const int64_t ts = NowMs();
    if (!cfg.range_auto) {
        for (int64_t start = 0; start < batch->num_rows(); start += cfg.range_rows) {
            const int64_t len = std::min<int64_t>(cfg.range_rows, batch->num_rows() - start);
            const size_t p = static_cast<size_t>((start / cfg.range_rows) % static_cast<int64_t>(n));
            const int rc = partitions[p]->Put(batch->Slice(start, len), ts);
            if (rc != 0) return rc;
        }
        return 0;
    }

    const int64_t total = batch->num_rows();
    const int64_t base = total / static_cast<int64_t>(n);
    const int64_t rem = total % static_cast<int64_t>(n);
    int64_t offset = 0;
    for (size_t p = 0; p < n; ++p) {
        const int64_t len = base + (static_cast<int64_t>(p) < rem ? 1 : 0);
        if (len <= 0) continue;
        const int rc = partitions[p]->Put(batch->Slice(offset, len), ts);
        if (rc != 0) return rc;
        offset += len;
    }
    return 0;
}

int DataframeDispatchStreamOperator::Work(IChannel* in, IChannel* out) {
    last_error_.clear();

    auto* src = dynamic_cast<IDataFrameChannel*>(in);
    if (!src) {
        SetError("DATAFRAME_DISPATCH_INVALID_SOURCE", "source channel is not dataframe");
        return -1;
    }

    std::vector<std::shared_ptr<IStreamChannel>> partitions;
    if (ResolveSinkPartitions(out, &partitions) != 0) return -1;

    EffectiveConfig effective;
    if (ResolveEffectiveConfig(cfg_, &effective) != 0) return -1;

    DataFrame data;
    if (src->Read(&data) != 0) {
        SetError("DATAFRAME_DISPATCH_READ_FAILED", "read dataframe failed");
        return -1;
    }

    int rc = 0;
    switch (effective.strategy) {
        case DispatchStrategy::kRoundRobin:
            rc = DispatchRoundRobin(data, effective, partitions);
            break;
        case DispatchStrategy::kHash:
            rc = DispatchHash(data, effective, partitions);
            break;
        case DispatchStrategy::kRange:
            rc = DispatchRange(data, effective, partitions);
            break;
    }
    if (rc != 0) {
        if (last_error_.empty()) {
            std::ostringstream oss;
            oss << "write stream_hub partition failed, rc=" << rc;
            SetError("DATAFRAME_DISPATCH_WRITE_FAILED", oss.str());
        }
        return -1;
    }

    auto* sink = dynamic_cast<IStreamChannel*>(out);
    if (sink) sink->CloseStream();
    return 0;
}

}  // namespace flowsql
