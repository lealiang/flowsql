/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "tcp_session_mock_stream_channel.h"

#include <arrow/api.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <thread>

namespace flowsql {

namespace {

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

}  // namespace

TcpSessionMockStreamChannel::TcpSessionMockStreamChannel(
    std::string category,
    std::string name,
    const TcpSessionMockOptions& options)
    : category_(std::move(category)),
      name_(std::move(name)),
      options_(options) {
    if (options_.batch_rows <= 0) options_.batch_rows = 64;
    if (options_.total_records < 0) options_.total_records = 0;
    if (options_.partition_count <= 0) options_.partition_count = 1;
    if (options_.queue_options.ring_size == 0) options_.queue_options.ring_size = 256;
    options_.queue_options.finite = true;
    if (options_.mode == TcpSessionMockMode::kStateless) {
        options_.queue_options.ring_mode = RingMode::SPMC;
    } else {
        options_.queue_options.ring_mode = RingMode::SPSC;
    }

    schema_ = arrow::schema({
        arrow::field("clientIP", arrow::utf8()),
        arrow::field("clientPort", arrow::int32()),
        arrow::field("serverIP", arrow::utf8()),
        arrow::field("serverPort", arrow::int32()),
        arrow::field("bps", arrow::int64()),
        arrow::field("pps", arrow::int64()),
    });
    queue_ = std::make_shared<RingStreamChannel>("ring", name_ + ".q", options_.queue_options);
}

TcpSessionMockStreamChannel::~TcpSessionMockStreamChannel() {
    Close();
}

int TcpSessionMockStreamChannel::Open() {
    std::lock_guard<std::mutex> lock(lifecycle_mu_);
    if (opened_.load(std::memory_order_acquire)) return 0;
    if (producer_thread_.joinable()) {
        producer_thread_.join();
    }
    queue_ = std::make_shared<RingStreamChannel>("ring", name_ + ".q", options_.queue_options);
    if (!queue_) return EINVAL;
    int rc = queue_->Open();
    if (rc != 0) return rc;

    stop_requested_.store(false, std::memory_order_release);
    finished_.store(false, std::memory_order_release);
    opened_.store(true, std::memory_order_release);
    producer_thread_ = std::thread(&TcpSessionMockStreamChannel::ProducerLoop, this);
    return 0;
}

int TcpSessionMockStreamChannel::Close() {
    Cancel();
    std::lock_guard<std::mutex> lock(lifecycle_mu_);
    if (queue_) {
        (void)queue_->Close();
    }
    opened_.store(false, std::memory_order_release);
    finished_.store(true, std::memory_order_release);
    return 0;
}

int TcpSessionMockStreamChannel::Put(std::shared_ptr<arrow::RecordBatch>, int64_t) {
    return ENOTSUP;
}

PollEvent TcpSessionMockStreamChannel::PollNext(int timeout_ms) {
    if (!queue_) return PollEvent::Error(-EINVAL, "mock channel queue unavailable");
    return queue_->PollNext(timeout_ms);
}

std::shared_ptr<arrow::Schema> TcpSessionMockStreamChannel::GetOutputSchema() {
    return schema_;
}

int TcpSessionMockStreamChannel::SetFilter(const char*,
                                           std::vector<std::string>* unsupported_out) {
    if (unsupported_out) unsupported_out->clear();
    return 0;
}

StreamChannelCapabilities TcpSessionMockStreamChannel::Capabilities() const {
    if (queue_) {
        StreamChannelCapabilities caps = queue_->Capabilities();
        caps.channel_type = ChannelType::kStream;
        caps.semantics.finite = true;
        caps.semantics.supports_filter_pushdown = false;
        caps.semantics.filter_requires_full_match = true;
        caps.partition.has_partition_id = (options_.mode == TcpSessionMockMode::kKeyed);
        caps.partition.supports_route_by_partition_id = (options_.mode == TcpSessionMockMode::kKeyed);
        caps.partition.preserves_partition_order = (options_.mode == TcpSessionMockMode::kKeyed);
        return caps;
    }
    StreamChannelCapabilities caps;
    caps.channel_type = ChannelType::kStream;
    caps.semantics.finite = true;
    caps.partition.has_partition_id = (options_.mode == TcpSessionMockMode::kKeyed);
    caps.partition.supports_route_by_partition_id = (options_.mode == TcpSessionMockMode::kKeyed);
    caps.partition.preserves_partition_order = (options_.mode == TcpSessionMockMode::kKeyed);
    return caps;
}

bool TcpSessionMockStreamChannel::IsFull() const {
    return queue_ && queue_->IsFull();
}

bool TcpSessionMockStreamChannel::IsEmpty() const {
    return !queue_ || queue_->IsEmpty();
}

size_t TcpSessionMockStreamChannel::Capacity() const {
    return queue_ ? queue_->Capacity() : 0;
}

size_t TcpSessionMockStreamChannel::Size() const {
    return queue_ ? queue_->Size() : 0;
}

void TcpSessionMockStreamChannel::CloseStream() {
    stop_requested_.store(true, std::memory_order_release);
    if (producer_thread_.joinable() &&
        producer_thread_.get_id() != std::this_thread::get_id()) {
        producer_thread_.join();
    }
    if (queue_) {
        queue_->CloseStream();
    }
    finished_.store(true, std::memory_order_release);
}

void TcpSessionMockStreamChannel::Cancel() {
    stop_requested_.store(true, std::memory_order_release);
    if (queue_) {
        queue_->Cancel();
    }
    if (producer_thread_.joinable() &&
        producer_thread_.get_id() != std::this_thread::get_id()) {
        producer_thread_.join();
    }
    finished_.store(true, std::memory_order_release);
    opened_.store(false, std::memory_order_release);
}

void TcpSessionMockStreamChannel::ProducerLoop() {
    const int64_t total = options_.total_records;
    const int32_t batch_rows = options_.batch_rows;
    int64_t produced = 0;
    int32_t batch_index = 0;

    while (!stop_requested_.load(std::memory_order_acquire) && produced < total) {
        const int32_t rows = static_cast<int32_t>(
            std::min<int64_t>(batch_rows, total - produced));
        int32_t partition_id = -1;
        if (options_.mode == TcpSessionMockMode::kKeyed) {
            partition_id = batch_index % std::max(options_.partition_count, 1);
        }

        auto batch = BuildBatch(produced, rows, partition_id);
        if (!batch) {
            if (queue_) queue_->Cancel();
            finished_.store(true, std::memory_order_release);
            opened_.store(false, std::memory_order_release);
            return;
        }

        const int64_t ts_ms = NowMs();
        while (!stop_requested_.load(std::memory_order_acquire)) {
            const int rc = queue_->Put(batch, ts_ms);
            if (rc == 0) break;
            if (rc == EAGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            stop_requested_.store(true, std::memory_order_release);
            break;
        }

        produced += rows;
        ++batch_index;
        if (options_.emit_interval_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options_.emit_interval_ms));
        }
    }

    if (!stop_requested_.load(std::memory_order_acquire) && queue_) {
        queue_->CloseStream();
    }
    finished_.store(true, std::memory_order_release);
    opened_.store(false, std::memory_order_release);
}

std::shared_ptr<arrow::RecordBatch> TcpSessionMockStreamChannel::BuildBatch(
    int64_t start_index,
    int32_t rows,
    int32_t partition_id) const {
    auto schema = schema_;
    if (partition_id >= 0) {
        auto meta = arrow::key_value_metadata(
            {"flowsql.partition_id"},
            {std::to_string(partition_id)});
        schema = schema_->WithMetadata(meta);
    }

    arrow::StringBuilder client_ip_builder;
    arrow::Int32Builder client_port_builder;
    arrow::StringBuilder server_ip_builder;
    arrow::Int32Builder server_port_builder;
    arrow::Int64Builder bps_builder;
    arrow::Int64Builder pps_builder;

    static const int32_t kServerPorts[4] = {80, 443, 8080, 8443};
    const int32_t partition_count = std::max(options_.partition_count, 1);
    for (int32_t i = 0; i < rows; ++i) {
        const int64_t global_index = start_index + i;
        int32_t client_id = static_cast<int32_t>(global_index % 64);
        if (options_.mode == TcpSessionMockMode::kKeyed && partition_id >= 0) {
            const int32_t slot = static_cast<int32_t>((global_index / partition_count) % 8);
            client_id = partition_id + slot * partition_count;
        }
        const int32_t server_id = static_cast<int32_t>(global_index % 4);
        const int32_t client_port = static_cast<int32_t>(10000 + (global_index % 2000));
        const int32_t server_port = kServerPorts[server_id];
        const int64_t bps = 1000 + (global_index % 100) * 100;
        const int64_t pps = 100 + (global_index % 100) * 10;

        if (!client_ip_builder.Append(MakeClientIp(client_id)).ok() ||
            !client_port_builder.Append(client_port).ok() ||
            !server_ip_builder.Append(MakeServerIp(server_id)).ok() ||
            !server_port_builder.Append(server_port).ok() ||
            !bps_builder.Append(bps).ok() ||
            !pps_builder.Append(pps).ok()) {
            return nullptr;
        }
    }

    auto client_ip = client_ip_builder.Finish().ValueOrDie();
    auto client_port = client_port_builder.Finish().ValueOrDie();
    auto server_ip = server_ip_builder.Finish().ValueOrDie();
    auto server_port = server_port_builder.Finish().ValueOrDie();
    auto bps = bps_builder.Finish().ValueOrDie();
    auto pps = pps_builder.Finish().ValueOrDie();

    return arrow::RecordBatch::Make(
        schema, rows,
        {client_ip, client_port, server_ip, server_port, bps, pps});
}

int64_t TcpSessionMockStreamChannel::NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string TcpSessionMockStreamChannel::MakeClientIp(int32_t client_id) {
    if (client_id < 0) client_id = 0;
    const int32_t octet2 = (client_id / 254) + 1;
    const int32_t octet3 = (client_id % 254) + 1;
    return "10." + std::to_string(octet2) + ".0." + std::to_string(octet3);
}

std::string TcpSessionMockStreamChannel::MakeServerIp(int32_t server_id) {
    if (server_id < 0) server_id = 0;
    return "172.16.0." + std::to_string((server_id % 4) + 1);
}

TcpSessionMockMode ParseTcpSessionMockMode(const std::string& mode) {
    const std::string m = ToLowerAscii(mode);
    if (m == "stateless") return TcpSessionMockMode::kStateless;
    if (m == "keyed") return TcpSessionMockMode::kKeyed;
    return TcpSessionMockMode::kNone;
}

}  // namespace flowsql
