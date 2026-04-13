/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>

#include <framework/builtin/stream/tcp_service_merge_stream_operator.h>
#include <framework/builtin/stream/tcp_session_mock_stream_channel.h>
#include <framework/core/dataframe_channel.h>
#include <framework/core/fan_in_stream_channel.h>
#include <framework/core/fan_out_stream_channel.h>
#include <framework/core/ring_stream_channel.h>
#include <framework/core/stream_channel_adapter.h>
#include <framework/core/stream_hub_channel.h>
#include <framework/interfaces/ichannel.h>
#include <framework/interfaces/idatabase_channel.h>
#include <framework/interfaces/istream_channel.h>
#include <framework/interfaces/istream_operator.h>
#include <services/scheduler/stream_runtime.h>
#include <services/scheduler/stream_task.h>

using namespace flowsql;
using namespace flowsql::scheduler;

#define ASSERT_TRUE(expr)                                                                \
    do {                                                                                 \
        if (!(expr)) {                                                                   \
            std::printf("[FAIL] %s:%d ASSERT_TRUE(%s)\n", __FILE__, __LINE__, #expr); \
            std::fflush(stdout);                                                         \
            assert(false);                                                               \
        }                                                                                \
    } while (0)

#define ASSERT_EQ(a, b)                                                                  \
    do {                                                                                 \
        auto _a = (a);                                                                   \
        auto _b = (b);                                                                   \
        if (!(_a == _b)) {                                                               \
            std::printf("[FAIL] %s:%d ASSERT_EQ(%s, %s)\n", __FILE__, __LINE__, #a, #b); \
            std::fflush(stdout);                                                         \
            assert(false);                                                               \
        }                                                                                \
    } while (0)

namespace {

template <typename Fn>
bool WaitUntil(Fn&& pred, int timeout_ms, int interval_ms = 1) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
    return pred();
}

std::shared_ptr<arrow::RecordBatch> MakeBatch(int64_t base,
                                               int64_t rows = 1,
                                               int32_t partition_id = -1) {
    auto field = arrow::field("v", arrow::int64());
    std::shared_ptr<arrow::Schema> schema;
    if (partition_id >= 0) {
        auto meta = arrow::key_value_metadata(
            {"flowsql.partition_id"},
            {std::to_string(partition_id)});
        schema = arrow::schema({field}, meta);
    } else {
        schema = arrow::schema({field});
    }

    arrow::Int64Builder builder;
    for (int64_t i = 0; i < rows; ++i) {
        (void)builder.Append(base + i);
    }
    auto arr = builder.Finish().ValueOrDie();
    return arrow::RecordBatch::Make(schema, rows, {arr});
}

std::shared_ptr<arrow::RecordBatch> MakeTcpSessionBatch(
    const std::vector<std::tuple<std::string, int32_t, std::string, int32_t, int64_t, int64_t>>& rows) {
    auto schema = arrow::schema({
        arrow::field("clientIP", arrow::utf8()),
        arrow::field("clientPort", arrow::int32()),
        arrow::field("serverIP", arrow::utf8()),
        arrow::field("serverPort", arrow::int32()),
        arrow::field("bps", arrow::int64()),
        arrow::field("pps", arrow::int64()),
    });

    arrow::StringBuilder client_ip_b;
    arrow::Int32Builder client_port_b;
    arrow::StringBuilder server_ip_b;
    arrow::Int32Builder server_port_b;
    arrow::Int64Builder bps_b;
    arrow::Int64Builder pps_b;

    for (const auto& row : rows) {
        (void)client_ip_b.Append(std::get<0>(row));
        (void)client_port_b.Append(std::get<1>(row));
        (void)server_ip_b.Append(std::get<2>(row));
        (void)server_port_b.Append(std::get<3>(row));
        (void)bps_b.Append(std::get<4>(row));
        (void)pps_b.Append(std::get<5>(row));
    }

    auto client_ip = client_ip_b.Finish().ValueOrDie();
    auto client_port = client_port_b.Finish().ValueOrDie();
    auto server_ip = server_ip_b.Finish().ValueOrDie();
    auto server_port = server_port_b.Finish().ValueOrDie();
    auto bps = bps_b.Finish().ValueOrDie();
    auto pps = pps_b.Finish().ValueOrDie();

    return arrow::RecordBatch::Make(
        schema,
        static_cast<int64_t>(rows.size()),
        {client_ip, client_port, server_ip, server_port, bps, pps});
}

void AssertTcpSessionSchema(const std::shared_ptr<arrow::Schema>& schema) {
    ASSERT_TRUE(schema != nullptr);
    ASSERT_TRUE(schema->GetFieldIndex("clientIP") >= 0);
    ASSERT_TRUE(schema->GetFieldIndex("clientPort") >= 0);
    ASSERT_TRUE(schema->GetFieldIndex("serverIP") >= 0);
    ASSERT_TRUE(schema->GetFieldIndex("serverPort") >= 0);
    ASSERT_TRUE(schema->GetFieldIndex("bps") >= 0);
    ASSERT_TRUE(schema->GetFieldIndex("pps") >= 0);
}

class DummyOutputChannel : public IChannel {
 public:
    DummyOutputChannel() = default;
    ~DummyOutputChannel() override = default;

    const char* Category() override { return "dummy"; }
    const char* Name() override { return "output"; }
    const char* Type() override { return ChannelType::kDataFrame; }
    const char* Schema() override { return "[]"; }

    int Open() override {
        opened_ = true;
        return 0;
    }
    int Close() override {
        opened_ = false;
        return 0;
    }
    bool IsOpened() const override { return opened_; }
    int Flush() override { return 0; }

 private:
    bool opened_ = true;
};

class MockBatchWriter final : public IBatchWriter {
 public:
    explicit MockBatchWriter(int64_t* rows_out) : rows_out_(rows_out) {}
    ~MockBatchWriter() override = default;

    int Write(const uint8_t* buf, size_t len) override {
        if (!buf || len == 0) return -1;
        auto arrow_buf = std::make_shared<arrow::Buffer>(buf, static_cast<int64_t>(len));
        auto open_res = arrow::ipc::RecordBatchStreamReader::Open(
            std::make_shared<arrow::io::BufferReader>(arrow_buf));
        if (!open_res.ok()) {
            last_error_ = open_res.status().ToString();
            return -1;
        }

        int64_t rows = 0;
        std::shared_ptr<arrow::RecordBatch> batch;
        while (true) {
            auto next = (*open_res)->ReadNext(&batch);
            if (!next.ok()) {
                last_error_ = next.ToString();
                return -1;
            }
            if (!batch) break;
            rows += batch->num_rows();
        }
        if (rows_out_) *rows_out_ += rows;
        return 0;
    }

    int Flush() override { return 0; }

    void Close(BatchWriteStats* stats) override {
        if (stats) {
            stats->rows_written = rows_out_ ? *rows_out_ : 0;
            stats->bytes_written = 0;
            stats->elapsed_ms = 0;
        }
    }

    const char* GetLastError() override {
        return last_error_.c_str();
    }

    void Release() override {
        delete this;
    }

 private:
    int64_t* rows_out_ = nullptr;
    std::string last_error_;
};

class MockDatabaseChannel final : public IDatabaseChannel {
 public:
    const char* Category() override { return "mockdb"; }
    const char* Name() override { return "stream_sink"; }
    const char* Type() override { return ChannelType::kDatabase; }
    const char* Schema() override { return "[]"; }

    int Open() override {
        opened_ = true;
        return 0;
    }

    int Close() override {
        opened_ = false;
        return 0;
    }

    bool IsOpened() const override { return opened_; }
    int Flush() override { return 0; }

    int CreateReader(const char*, IBatchReader**) override { return -1; }

    int CreateWriter(const char* table, IBatchWriter** writer) override {
        if (!opened_ || !writer || !table || table[0] == '\0') return -1;
        tables_.push_back(table);
        *writer = new MockBatchWriter(&rows_written_);
        return 0;
    }

    int CreateArrowReader(const char*, IArrowReader**) override { return -1; }
    int CreateArrowWriter(const char*, IArrowWriter**) override { return -1; }
    int ExecuteQueryArrow(const char*,
                          std::vector<std::shared_ptr<arrow::RecordBatch>>*) override { return -1; }

    int WriteArrowBatches(const char* table,
                          const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) override {
        if (force_arrow_failure_) return -1;
        if (!opened_ || !table || table[0] == '\0') return -1;
        tables_.push_back(table);
        for (const auto& b : batches) {
            if (b) rows_written_ += b->num_rows();
        }
        return 0;
    }

    int ExecuteSql(const char*) override { return 0; }
    const char* GetLastError() override { return last_error_.c_str(); }
    bool IsConnected() override { return opened_; }

    void SetForceArrowFailure(bool on) { force_arrow_failure_ = on; }
    int64_t RowsWritten() const { return rows_written_; }
    size_t WriteCalls() const { return tables_.size(); }
    std::string LastTable() const { return tables_.empty() ? "" : tables_.back(); }

 private:
    bool opened_ = true;
    bool force_arrow_failure_ = false;
    int64_t rows_written_ = 0;
    std::vector<std::string> tables_;
    std::string last_error_;
};

class MockStreamOperator : public IStreamOperator {
 public:
    MockStreamOperator(ParallelStrategy strategy = ParallelStrategy::NONE,
                       int parallelism = 1,
                       int process_delay_ms = 0,
                       std::string partition_spec = "")
        : strategy_(strategy),
          parallelism_(parallelism),
          process_delay_ms_(process_delay_ms),
          partition_spec_(std::move(partition_spec)) {}

    std::string Category() override { return "test"; }
    std::string Name() override { return "mock_stream_op"; }
    std::string Description() override { return "test stream operator"; }

    int Configure(const char*, const char*) override { return 0; }

    int Init(const char*, const StreamSinkContext& sink_ctx) override {
        output_ = sink_ctx.sink_channel;
        return 0;
    }

    int OnSchemaReady(std::shared_ptr<arrow::Schema>) override {
        on_schema_calls_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    int Process(const arrow::RecordBatch& batch, int64_t) override {
        in_process_.store(true, std::memory_order_release);
        if (process_delay_ms_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(process_delay_ms_));
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            threads_.insert(std::this_thread::get_id());
        }
        process_calls_.fetch_add(1, std::memory_order_relaxed);
        processed_rows_.fetch_add(static_cast<uint64_t>(batch.num_rows()), std::memory_order_relaxed);
        in_process_.store(false, std::memory_order_release);
        return 0;
    }

    int Tick(int64_t) override {
        tick_calls_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    int Flush() override {
        flush_calls_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    std::string GetStats() override {
        return "{}";
    }

    std::string LastError() override {
        return last_error_;
    }

    ParallelStrategy GetParallelStrategy() const override {
        return strategy_;
    }

    std::string GetPartitionSpec() const override {
        return partition_spec_;
    }

    int GetParallelism() const override {
        return parallelism_;
    }

    uint64_t ProcessCalls() const {
        return process_calls_.load(std::memory_order_relaxed);
    }

    uint64_t ProcessedRows() const {
        return processed_rows_.load(std::memory_order_relaxed);
    }

    uint64_t TickCalls() const {
        return tick_calls_.load(std::memory_order_relaxed);
    }

    uint64_t FlushCalls() const {
        return flush_calls_.load(std::memory_order_relaxed);
    }

    uint64_t OnSchemaCalls() const {
        return on_schema_calls_.load(std::memory_order_relaxed);
    }

    bool IsInProcess() const {
        return in_process_.load(std::memory_order_acquire);
    }

    size_t WorkerThreadCount() const {
        std::lock_guard<std::mutex> lock(mu_);
        return threads_.size();
    }

 private:
    ParallelStrategy strategy_ = ParallelStrategy::NONE;
    int parallelism_ = 1;
    int process_delay_ms_ = 0;
    std::string partition_spec_;

    IChannel* output_ = nullptr;
    std::string last_error_;

    std::atomic<bool> in_process_{false};
    std::atomic<uint64_t> on_schema_calls_{0};
    std::atomic<uint64_t> process_calls_{0};
    std::atomic<uint64_t> processed_rows_{0};
    std::atomic<uint64_t> tick_calls_{0};
    std::atomic<uint64_t> flush_calls_{0};

    mutable std::mutex mu_;
    std::unordered_set<std::thread::id> threads_;
};

void TestT1RingReadWrite() {
    RingStreamChannelOptions opts;
    opts.ring_size = 8;
    opts.ring_mode = RingMode::SPSC;
    opts.overflow = OverflowPolicy::kDrop;

    RingStreamChannel ch("ring", "t1", opts);
    ASSERT_EQ(ch.Open(), 0);

    ASSERT_EQ(ch.Put(MakeBatch(1), 1001), 0);
    ASSERT_EQ(ch.Put(MakeBatch(2), 1002), 0);

    PollEvent ev1 = ch.PollNext(0);
    PollEvent ev2 = ch.PollNext(0);
    ASSERT_EQ(ev1.kind, PollEventKind::kData);
    ASSERT_EQ(ev2.kind, PollEventKind::kData);
    ASSERT_EQ(ev1.batch.ts_ms, 1001);
    ASSERT_EQ(ev2.batch.ts_ms, 1002);
    ASSERT_TRUE(ev1.batch.data != nullptr);
    ASSERT_TRUE(ev2.batch.data != nullptr);
    ASSERT_EQ(ev1.batch.data->num_rows(), 1);
    ASSERT_EQ(ev2.batch.data->num_rows(), 1);

    ch.CloseStream();
    PollEvent ev3 = ch.PollNext(10);
    ASSERT_EQ(ev3.kind, PollEventKind::kEof);
    ASSERT_EQ(ch.Close(), 0);
}

void TestT2Backpressure() {
    {
        RingStreamChannelOptions opts;
        opts.ring_size = 2;
        opts.ring_mode = RingMode::SPSC;
        opts.overflow = OverflowPolicy::kDrop;

        RingStreamChannel ch("ring", "t2_drop", opts);
        ASSERT_EQ(ch.Open(), 0);
        ASSERT_EQ(ch.Put(MakeBatch(1), 1), 0);
        ASSERT_EQ(ch.Put(MakeBatch(2), 2), 0);
        ASSERT_EQ(ch.Put(MakeBatch(3), 3), EAGAIN);
        ASSERT_EQ(ch.Close(), 0);
    }

    {
        RingStreamChannelOptions opts;
        opts.ring_size = 2;
        opts.ring_mode = RingMode::SPSC;
        opts.overflow = OverflowPolicy::kBlock;
        opts.finite = true;

        RingStreamChannel ch("ring", "t2_block", opts);
        ASSERT_EQ(ch.Open(), 0);
        ASSERT_EQ(ch.Put(MakeBatch(1), 1), 0);
        ASSERT_EQ(ch.Put(MakeBatch(2), 2), 0);

        auto fut = std::async(std::launch::async, [&ch]() {
            return ch.Put(MakeBatch(3), 3);
        });
        ASSERT_EQ(fut.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);

        PollEvent ev = ch.PollNext(0);
        ASSERT_EQ(ev.kind, PollEventKind::kData);

        ASSERT_EQ(fut.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
        ASSERT_EQ(fut.get(), 0);
        ASSERT_EQ(ch.Close(), 0);
    }
}

void TestT3EofPropagation() {
    RingStreamChannelOptions opts;
    opts.ring_size = 8;
    opts.ring_mode = RingMode::SPSC;

    RingStreamChannel ch("ring", "t3", opts);
    ASSERT_EQ(ch.Open(), 0);
    ASSERT_EQ(ch.Put(MakeBatch(1), 10), 0);
    ASSERT_EQ(ch.Put(MakeBatch(2), 20), 0);

    ch.CloseStream();

    int data_count = 0;
    for (;;) {
        PollEvent ev = ch.PollNext(50);
        if (ev.kind == PollEventKind::kData) {
            data_count++;
            continue;
        }
        ASSERT_EQ(ev.kind, PollEventKind::kEof);
        break;
    }
    ASSERT_EQ(data_count, 2);
    ASSERT_EQ(ch.Close(), 0);
}

void TestT4FanInMerge() {
    RingStreamChannelOptions opts;
    opts.ring_size = 16;
    opts.ring_mode = RingMode::SPSC;

    auto s1 = std::make_shared<RingStreamChannel>("ring", "t4_s1", opts);
    auto s2 = std::make_shared<RingStreamChannel>("ring", "t4_s2", opts);
    std::vector<std::shared_ptr<IStreamChannel>> sources = {s1, s2};

    FanInStreamChannel fanin("fanin", "t4", sources);
    ASSERT_EQ(fanin.Open(), 0);

    ASSERT_EQ(s1->Put(MakeBatch(100), 101), 0);
    ASSERT_EQ(s2->Put(MakeBatch(200), 201), 0);
    s1->CloseStream();
    ASSERT_EQ(s2->Put(MakeBatch(300), 202), 0);
    s2->CloseStream();

    std::set<int64_t> ts;
    int rows = 0;
    for (;;) {
        PollEvent ev = fanin.PollNext(200);
        if (ev.kind == PollEventKind::kData) {
            rows += ev.batch.data ? ev.batch.data->num_rows() : 0;
            ts.insert(ev.batch.ts_ms);
            continue;
        }
        ASSERT_TRUE(ev.kind == PollEventKind::kEof || ev.kind == PollEventKind::kDrainedAfterCancel);
        break;
    }

    ASSERT_EQ(rows, 3);
    ASSERT_TRUE(ts.count(101) == 1);
    ASSERT_TRUE(ts.count(201) == 1);
    ASSERT_TRUE(ts.count(202) == 1);
    ASSERT_EQ(fanin.Close(), 0);
}

void TestT5NoneScenario() {
    StreamRuntime runtime;
    runtime.Start(1);

    auto source = std::make_shared<RingStreamChannel>("ring", "t5", RingStreamChannelOptions{});
    ASSERT_EQ(source->Open(), 0);

    auto op = std::make_shared<MockStreamOperator>(ParallelStrategy::NONE, 1);
    auto output = std::make_shared<DummyOutputChannel>();
    auto task = std::make_shared<StreamTask>("t5", &runtime);
    task->PrepareForRun(1, 1);

    auto shard = std::make_shared<ShardRunner>(0, source, op, output, task.get());
    task->AddShard(shard);
    ASSERT_TRUE(runtime.TrySchedule(shard));

    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(source->Put(MakeBatch(i), 100 + i), 0);
    }
    source->CloseStream();

    task->Join();
    TaskSnapshot snap = task->Snapshot();
    ASSERT_EQ(snap.status, StreamTaskStatus::kStopped);
    ASSERT_EQ(op->ProcessedRows(), 10);
    ASSERT_EQ(op->OnSchemaCalls(), 1);

    runtime.Stop();
}

void TestT6StatelessScenarioSpmc() {
    StreamRuntime runtime;
    runtime.Start(4);

    RingStreamChannelOptions opts;
    opts.ring_mode = RingMode::SPMC;
    opts.ring_size = 4096;
    opts.overflow = OverflowPolicy::kDrop;
    auto source = std::make_shared<RingStreamChannel>("ring", "t6_spmc", opts);
    ASSERT_EQ(source->Open(), 0);

    auto output = std::make_shared<DummyOutputChannel>();
    auto task = std::make_shared<StreamTask>("t6", &runtime);
    const uint32_t shard_n = 4;
    task->PrepareForRun(shard_n, 1);

    std::vector<std::shared_ptr<MockStreamOperator>> ops;
    for (uint32_t i = 0; i < shard_n; ++i) {
        auto op = std::make_shared<MockStreamOperator>(ParallelStrategy::STATELESS, shard_n, 1);
        auto shard = std::make_shared<ShardRunner>(i, source, op, output, task.get());
        task->AddShard(shard);
        ops.push_back(op);
        ASSERT_TRUE(runtime.TrySchedule(shard));
    }

    constexpr int kBatches = 400;
    for (int i = 0; i < kBatches; ++i) {
        ASSERT_EQ(source->Put(MakeBatch(i), i), 0);
    }
    source->CloseStream();

    task->Join();

    uint64_t total_rows = 0;
    int active_ops = 0;
    size_t total_threads = 0;
    for (const auto& op : ops) {
        total_rows += op->ProcessedRows();
        if (op->ProcessCalls() > 0) active_ops++;
        total_threads += op->WorkerThreadCount();
    }

    ASSERT_EQ(total_rows, static_cast<uint64_t>(kBatches));
    ASSERT_TRUE(active_ops >= 2);
    ASSERT_TRUE(total_threads >= 2);

    runtime.Stop();
}

void TestT7StatelessTermination() {
    StreamRuntime runtime;
    runtime.Start(3);

    RingStreamChannelOptions opts;
    opts.ring_mode = RingMode::SPMC;
    opts.ring_size = 128;
    auto source = std::make_shared<RingStreamChannel>("ring", "t7_spmc", opts);
    ASSERT_EQ(source->Open(), 0);

    auto output = std::make_shared<DummyOutputChannel>();
    auto task = std::make_shared<StreamTask>("t7", &runtime);
    task->PrepareForRun(3, 1);

    std::vector<std::shared_ptr<MockStreamOperator>> ops;
    for (uint32_t i = 0; i < 3; ++i) {
        auto op = std::make_shared<MockStreamOperator>(ParallelStrategy::STATELESS, 3);
        auto shard = std::make_shared<ShardRunner>(i, source, op, output, task.get());
        task->AddShard(shard);
        ops.push_back(op);
        ASSERT_TRUE(runtime.TrySchedule(shard));
    }

    for (int i = 0; i < 9; ++i) {
        ASSERT_EQ(source->Put(MakeBatch(i), i), 0);
    }
    source->CloseStream();

    task->Join();
    TaskSnapshot s = task->Snapshot();
    ASSERT_EQ(s.status, StreamTaskStatus::kStopped);
    ASSERT_EQ(s.active_shards, 0);
    ASSERT_EQ(s.processed_rows, 9);

    runtime.Stop();
}

void TestT8KeyedScenario() {
    StreamRuntime runtime;
    runtime.Start(3);

    RingStreamChannelOptions src_opts;
    src_opts.ring_mode = RingMode::SPSC;
    src_opts.ring_size = 128;
    auto source = std::make_shared<RingStreamChannel>("ring", "t8_src", src_opts);

    RingStreamChannelOptions part_opts;
    part_opts.ring_mode = RingMode::SPSC;
    part_opts.ring_size = 64;

    auto fanout = std::make_shared<FanOutStreamChannel>(
        "fanout", "t8_fanout", source, 3,
        FanOutMode::ROUTE_BY_PARTITION_ID,
        R"({"mode":"upstream_partition_id"})",
        part_opts);

    ASSERT_EQ(fanout->Open(), 0);

    auto output = std::make_shared<DummyOutputChannel>();
    auto task = std::make_shared<StreamTask>("t8", &runtime);
    task->PrepareForRun(3, 1);

    std::vector<std::shared_ptr<MockStreamOperator>> ops;
    for (uint32_t i = 0; i < 3; ++i) {
        auto op = std::make_shared<MockStreamOperator>(ParallelStrategy::KEYED, 3);
        auto input = fanout->GetPartition(i);
        ASSERT_TRUE(input != nullptr);
        auto shard = std::make_shared<ShardRunner>(i, input, op, output, task.get());
        task->AddShard(shard);
        ops.push_back(op);
        ASSERT_TRUE(runtime.TrySchedule(shard));
    }

    for (int i = 0; i < 9; ++i) {
        const int p = i % 3;
        ASSERT_EQ(source->Put(MakeBatch(i, 1, p), i), 0);
    }
    source->CloseStream();

    task->Join();
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(ops[i]->ProcessedRows(), 3);
    }

    fanout->Close();
    runtime.Stop();
}

void TestT9RunnerStepBudgetYield() {
    auto source = std::make_shared<RingStreamChannel>("ring", "t9", RingStreamChannelOptions{});
    ASSERT_EQ(source->Open(), 0);

    for (int i = 0; i < 20; ++i) {
        ASSERT_EQ(source->Put(MakeBatch(i), i), 0);
    }

    auto op = std::make_shared<MockStreamOperator>();
    auto output = std::make_shared<DummyOutputChannel>();
    ShardRunner shard(0, source, op, output, nullptr);

    int rc = shard.Step();
    ASSERT_EQ(rc, kStepYield);
    ASSERT_EQ(op->ProcessCalls(), 8);

    source->Cancel();
    (void)shard.Finalize();
}

void TestT10StreamTaskLifecycleCancel() {
    StreamRuntime runtime;
    runtime.Start(1);

    auto source = std::make_shared<RingStreamChannel>("ring", "t10", RingStreamChannelOptions{});
    ASSERT_EQ(source->Open(), 0);

    auto op = std::make_shared<MockStreamOperator>();
    auto output = std::make_shared<DummyOutputChannel>();
    auto task = std::make_shared<StreamTask>("t10", &runtime);
    task->PrepareForRun(1, 1);

    auto shard = std::make_shared<ShardRunner>(0, source, op, output, task.get());
    task->AddShard(shard);
    ASSERT_TRUE(runtime.TrySchedule(shard));

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    task->RequestStop();
    task->Join();

    TaskSnapshot s = task->Snapshot();
    ASSERT_EQ(s.status, StreamTaskStatus::kStopped);
    ASSERT_TRUE(s.joined);
    ASSERT_EQ(s.active_shards, 0);

    runtime.Stop();
}

void TestT11CancelTimeoutFallback() {
    RingStreamChannelOptions opts;
    opts.ring_size = 2;
    opts.ring_mode = RingMode::SPSC;
    opts.overflow = OverflowPolicy::kDrop;
    auto source = std::make_shared<RingStreamChannel>("ring", "t11", opts);
    ASSERT_EQ(source->Open(), 0);

    int64_t put_ts = 1;
    while (!source->IsFull() && put_ts <= 16) {
        ASSERT_EQ(source->Put(MakeBatch(put_ts), put_ts), 0);
        ++put_ts;
    }
    ASSERT_TRUE(source->IsFull());

    const auto begin = std::chrono::steady_clock::now();
    source->Cancel();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin).count();

    ASSERT_TRUE(elapsed_ms >= 4500);

    int drained = 0;
    for (;;) {
        PollEvent ev = source->PollNext(10);
        if (ev.kind == PollEventKind::kData) {
            ++drained;
            continue;
        }
        ASSERT_EQ(ev.kind, PollEventKind::kDrainedAfterCancel);
        break;
    }
    ASSERT_TRUE(drained >= 1);

    source->Close();
}

void TestT12DynamicSchemaEmptyStream() {
    StreamRuntime runtime;
    runtime.Start(1);

    RingStreamChannelOptions opts;
    opts.static_schema.reset();
    auto source = std::make_shared<RingStreamChannel>("ring", "t12", opts);
    ASSERT_EQ(source->Open(), 0);

    auto op = std::make_shared<MockStreamOperator>();
    auto output = std::make_shared<DummyOutputChannel>();
    auto task = std::make_shared<StreamTask>("t12", &runtime);
    task->PrepareForRun(1, 1);

    auto shard = std::make_shared<ShardRunner>(0, source, op, output, task.get());
    task->AddShard(shard);
    ASSERT_TRUE(runtime.TrySchedule(shard));

    source->CloseStream();
    task->Join();

    ASSERT_EQ(op->OnSchemaCalls(), 0);
    ASSERT_EQ(op->FlushCalls(), 1);
    ASSERT_EQ(task->Snapshot().status, StreamTaskStatus::kStopped);

    runtime.Stop();
}

void TestT13TaskSnapshotAggregation() {
    StreamRuntime runtime;
    runtime.Start(2);

    auto s1 = std::make_shared<RingStreamChannel>("ring", "t13_s1", RingStreamChannelOptions{});
    auto s2 = std::make_shared<RingStreamChannel>("ring", "t13_s2", RingStreamChannelOptions{});
    ASSERT_EQ(s1->Open(), 0);
    ASSERT_EQ(s2->Open(), 0);

    auto op1 = std::make_shared<MockStreamOperator>();
    auto op2 = std::make_shared<MockStreamOperator>();
    auto output = std::make_shared<DummyOutputChannel>();
    auto task = std::make_shared<StreamTask>("t13", &runtime);
    task->PrepareForRun(2, 1);

    auto shard1 = std::make_shared<ShardRunner>(0, s1, op1, output, task.get());
    auto shard2 = std::make_shared<ShardRunner>(1, s2, op2, output, task.get());
    task->AddShard(shard1);
    task->AddShard(shard2);
    ASSERT_TRUE(runtime.TrySchedule(shard1));
    ASSERT_TRUE(runtime.TrySchedule(shard2));

    for (int i = 0; i < 3; ++i) ASSERT_EQ(s1->Put(MakeBatch(i), i), 0);
    for (int i = 0; i < 5; ++i) ASSERT_EQ(s2->Put(MakeBatch(i + 100), i + 100), 0);
    s1->CloseStream();
    s2->CloseStream();

    task->Join();

    TaskSnapshot snap = task->Snapshot();
    ASSERT_EQ(snap.status, StreamTaskStatus::kStopped);
    ASSERT_EQ(snap.active_shards, 0);
    ASSERT_EQ(snap.processed_batches, 8);
    ASSERT_EQ(snap.processed_rows, 8);
    ASSERT_EQ(snap.poll_errors, 0);

    runtime.Stop();
}

void TestT14StateMachineTransitions() {
    StreamRuntime runtime;
    runtime.Start(1);

    RingStreamChannelOptions opts;
    opts.ring_mode = RingMode::SPSC;
    opts.ring_size = 64;
    auto source = std::make_shared<RingStreamChannel>("ring", "t14", opts);
    ASSERT_EQ(source->Open(), 0);

    auto op = std::make_shared<MockStreamOperator>(ParallelStrategy::NONE, 1, 120);
    auto output = std::make_shared<DummyOutputChannel>();
    auto task = std::make_shared<StreamTask>("t14", &runtime);
    task->PrepareForRun(1, 1);

    auto shard = std::make_shared<ShardRunner>(0, source, op, output, task.get());
    task->AddShard(shard);
    ASSERT_TRUE(runtime.TrySchedule(shard));

    ASSERT_EQ(source->Put(MakeBatch(1), 1), 0);
    ASSERT_TRUE(WaitUntil([&]() { return op->IsInProcess(); }, 1000));

    const bool marked_pending = runtime.TrySchedule(shard);
    ASSERT_TRUE(marked_pending);
    ASSERT_TRUE(WaitUntil([&]() {
        return shard->exec_state.load(std::memory_order_acquire) == ShardExecState::kRunningPending;
    }, 300));

    std::atomic<bool> stop_sampler{false};
    std::atomic<bool> saw_waiting_retry{false};
    std::thread sampler([&]() {
        while (!stop_sampler.load(std::memory_order_acquire)) {
            if (shard->exec_state.load(std::memory_order_acquire) == ShardExecState::kWaitingRetry) {
                saw_waiting_retry.store(true, std::memory_order_release);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    ASSERT_TRUE(WaitUntil([&]() { return op->ProcessCalls() >= 1; }, 2000));
    ASSERT_TRUE(WaitUntil([&]() { return op->TickCalls() >= 1; }, 2000));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop_sampler.store(true, std::memory_order_release);
    sampler.join();

    task->RequestStop();
    task->Join();

    ASSERT_TRUE(saw_waiting_retry.load(std::memory_order_acquire));
    ASSERT_TRUE(op->TickCalls() >= 1);

    runtime.Stop();
}

void TestT15DoneAbsorption() {
    StreamRuntime runtime;
    runtime.Start(1);

    auto source = std::make_shared<RingStreamChannel>("ring", "t15", RingStreamChannelOptions{});
    ASSERT_EQ(source->Open(), 0);

    auto op = std::make_shared<MockStreamOperator>();
    auto output = std::make_shared<DummyOutputChannel>();
    auto task = std::make_shared<StreamTask>("t15", &runtime);
    task->PrepareForRun(1, 1);

    auto shard = std::make_shared<ShardRunner>(0, source, op, output, task.get());
    task->AddShard(shard);
    ASSERT_TRUE(runtime.TrySchedule(shard));

    source->CloseStream();
    task->Join();

    TaskSnapshot s1 = task->Snapshot();
    ASSERT_EQ(s1.active_shards, 0);

    ASSERT_TRUE(!runtime.TrySchedule(shard));

    shard->MarkDone();
    shard->MarkDone();

    TaskSnapshot s2 = task->Snapshot();
    ASSERT_EQ(s2.active_shards, 0);

    runtime.Stop();
}

void TestT16TcpSessionMockThreeModes() {
    struct ModeCase {
        TcpSessionMockMode mode;
        const char* name;
    };
    const std::vector<ModeCase> modes = {
        {TcpSessionMockMode::kNone, "none"},
        {TcpSessionMockMode::kStateless, "stateless"},
        {TcpSessionMockMode::kKeyed, "keyed"},
    };

    for (const auto& item : modes) {
        TcpSessionMockOptions opts;
        opts.mode = item.mode;
        opts.total_records = 37;
        opts.batch_rows = 8;
        opts.emit_interval_ms = 0;
        opts.partition_count = 4;
        opts.queue_options.ring_size = 128;
        opts.queue_options.overflow = OverflowPolicy::kDrop;

        TcpSessionMockStreamChannel ch("tcp_session_mock", std::string("t16_") + item.name, opts);
        ASSERT_EQ(ch.Open(), 0);

        int64_t total_rows = 0;
        bool seen_keyed_meta = false;
        for (int i = 0; i < 200; ++i) {
            PollEvent ev = ch.PollNext(50);
            if (ev.kind == PollEventKind::kTimeout) continue;
            if (ev.kind == PollEventKind::kError) {
                ASSERT_TRUE(false);
            }
            if (ev.kind == PollEventKind::kEof || ev.kind == PollEventKind::kDrainedAfterCancel) {
                break;
            }
            ASSERT_EQ(ev.kind, PollEventKind::kData);
            ASSERT_TRUE(ev.batch.data != nullptr);
            AssertTcpSessionSchema(ev.batch.data->schema());
            total_rows += ev.batch.data->num_rows();
            if (item.mode == TcpSessionMockMode::kKeyed) {
                auto schema = ev.batch.data->schema();
                auto meta = schema ? schema->metadata() : nullptr;
                ASSERT_TRUE(meta != nullptr);
                const int idx = meta->FindKey("flowsql.partition_id");
                ASSERT_TRUE(idx >= 0);
                seen_keyed_meta = true;
            }
        }

        ASSERT_EQ(total_rows, opts.total_records);
        if (item.mode == TcpSessionMockMode::kKeyed) {
            ASSERT_TRUE(seen_keyed_meta);
        }
        ASSERT_EQ(ch.Close(), 0);
    }
}

void TestT17TcpServiceMergeOperator() {
    RingStreamChannelOptions out_opts;
    out_opts.ring_size = 128;
    out_opts.ring_mode = RingMode::SPSC;
    auto output = std::make_shared<RingStreamChannel>("ring", "t17_out", out_opts);
    ASSERT_EQ(output->Open(), 0);

    TcpServiceMergeStreamOperator op;
    StreamSinkContext sink_ctx;
    sink_ctx.sink_channel = output.get();
    sink_ctx.sink_type = ChannelType::kStream;
    sink_ctx.into_raw = "stream.t17_out";
    ASSERT_EQ(op.Init("{}", sink_ctx), 0);

    auto input_schema = arrow::schema({
        arrow::field("clientIP", arrow::utf8()),
        arrow::field("clientPort", arrow::int32()),
        arrow::field("serverIP", arrow::utf8()),
        arrow::field("serverPort", arrow::int32()),
        arrow::field("bps", arrow::int64()),
        arrow::field("pps", arrow::int64()),
    });
    ASSERT_EQ(op.OnSchemaReady(input_schema), 0);

    auto b1 = MakeTcpSessionBatch({
        {"10.0.0.1", 10000, "172.16.0.1", 80, 100, 10},
        {"10.0.0.1", 10001, "172.16.0.1", 80, 200, 20},
        {"10.0.0.1", 10002, "172.16.0.2", 443, 300, 30},
        {"10.0.0.2", 10003, "172.16.0.1", 80, 400, 40},
    });
    auto b2 = MakeTcpSessionBatch({
        {"10.0.0.1", 10004, "172.16.0.1", 80, 500, 50},
        {"10.0.0.2", 10005, "172.16.0.1", 80, 600, 60},
        {"10.0.0.2", 10006, "172.16.0.3", 8080, 700, 70},
    });

    ASSERT_EQ(op.Process(*b1, 1001), 0);
    ASSERT_EQ(op.Process(*b2, 1002), 0);
    ASSERT_EQ(op.Flush(), 0);
    output->CloseStream();

    std::unordered_map<std::string, std::pair<int64_t, int64_t>> agg;
    for (int i = 0; i < 100; ++i) {
        PollEvent ev = output->PollNext(50);
        if (ev.kind == PollEventKind::kTimeout) continue;
        if (ev.kind == PollEventKind::kEof || ev.kind == PollEventKind::kDrainedAfterCancel) break;
        ASSERT_EQ(ev.kind, PollEventKind::kData);
        ASSERT_TRUE(ev.batch.data != nullptr);

        auto schema = ev.batch.data->schema();
        ASSERT_TRUE(schema != nullptr);
        ASSERT_TRUE(schema->GetFieldIndex("clientPort") < 0);
        ASSERT_TRUE(schema->GetFieldIndex("clientIP") >= 0);
        ASSERT_TRUE(schema->GetFieldIndex("serverIP") >= 0);
        ASSERT_TRUE(schema->GetFieldIndex("serverPort") >= 0);
        ASSERT_TRUE(schema->GetFieldIndex("bps") >= 0);
        ASSERT_TRUE(schema->GetFieldIndex("pps") >= 0);

        auto client_ip = std::static_pointer_cast<arrow::StringArray>(ev.batch.data->column(0));
        auto server_ip = std::static_pointer_cast<arrow::StringArray>(ev.batch.data->column(1));
        auto server_port = std::static_pointer_cast<arrow::Int32Array>(ev.batch.data->column(2));
        auto bps = std::static_pointer_cast<arrow::Int64Array>(ev.batch.data->column(3));
        auto pps = std::static_pointer_cast<arrow::Int64Array>(ev.batch.data->column(4));
        for (int64_t r = 0; r < ev.batch.data->num_rows(); ++r) {
            const std::string key = client_ip->GetString(r) + "|" +
                                    server_ip->GetString(r) + "|" +
                                    std::to_string(server_port->Value(r));
            agg[key].first += bps->Value(r);
            agg[key].second += pps->Value(r);
        }
    }

    ASSERT_EQ(agg.size(), size_t(4));
    ASSERT_EQ(agg["10.0.0.1|172.16.0.1|80"].first, 800);
    ASSERT_EQ(agg["10.0.0.1|172.16.0.1|80"].second, 80);
    ASSERT_EQ(agg["10.0.0.1|172.16.0.2|443"].first, 300);
    ASSERT_EQ(agg["10.0.0.1|172.16.0.2|443"].second, 30);
    ASSERT_EQ(agg["10.0.0.2|172.16.0.1|80"].first, 1000);
    ASSERT_EQ(agg["10.0.0.2|172.16.0.1|80"].second, 100);
    ASSERT_EQ(agg["10.0.0.2|172.16.0.3|8080"].first, 700);
    ASSERT_EQ(agg["10.0.0.2|172.16.0.3|8080"].second, 70);

    output->Close();
}

void TestT18StreamChannelAdapterDataFrameAppend() {
    auto df_sink = std::make_shared<DataFrameChannel>("dataframe", "t18_sink");
    ASSERT_EQ(df_sink->Open(), 0);

    auto append_sink = std::static_pointer_cast<IAppendableDataFrameChannel>(df_sink);
    auto adapter = StreamChannelAdapter::MakeDataFrameAppend(
        "adapter", "t18_df_adapter", append_sink);
    ASSERT_TRUE(adapter != nullptr);

    ASSERT_EQ(adapter->Put(MakeBatch(1, 2), 1001), 0);
    ASSERT_EQ(adapter->Put(MakeBatch(10, 3), 1002), 0);

    DataFrame out;
    ASSERT_EQ(df_sink->Read(&out), 0);
    ASSERT_EQ(out.RowCount(), 5);
    ASSERT_EQ(out.GetSchema().size(), size_t(1));
    ASSERT_EQ(out.GetSchema()[0].name, "v");

    auto bad_schema = arrow::schema({arrow::field("x", arrow::int64())});
    arrow::Int64Builder b;
    (void)b.Append(42);
    auto bad_arr = b.Finish().ValueOrDie();
    auto bad_batch = arrow::RecordBatch::Make(bad_schema, 1, {bad_arr});
    ASSERT_TRUE(adapter->Put(bad_batch, 1003) != 0);

    df_sink->Close();
}

void TestT19StreamChannelAdapterDatabaseWriter() {
    auto db_sink = std::make_shared<MockDatabaseChannel>();
    ASSERT_EQ(db_sink->Open(), 0);

    auto adapter = StreamChannelAdapter::MakeDatabaseWriter(
        "adapter", "t19_db_adapter", db_sink, "svc_access");
    ASSERT_TRUE(adapter != nullptr);

    // 先覆盖 Arrow 直写路径，再覆盖回退的 CreateWriter 路径。
    ASSERT_EQ(adapter->Put(MakeBatch(1, 4), 2001), 0);
    db_sink->SetForceArrowFailure(true);
    ASSERT_EQ(adapter->Put(MakeBatch(10, 3), 2002), 0);

    ASSERT_EQ(adapter->Flush(), 0);
    ASSERT_EQ(db_sink->WriteCalls(), size_t(2));
    ASSERT_EQ(db_sink->RowsWritten(), int64_t(7));
    ASSERT_EQ(db_sink->LastTable(), std::string("svc_access"));
}

void TestT20StreamChannelAdapterOutputOnlyContract() {
    auto df_sink = std::make_shared<DataFrameChannel>("dataframe", "t20_sink");
    ASSERT_EQ(df_sink->Open(), 0);
    auto append_sink = std::static_pointer_cast<IAppendableDataFrameChannel>(df_sink);
    auto adapter = StreamChannelAdapter::MakeDataFrameAppend(
        "adapter", "t20_df_adapter", append_sink);
    ASSERT_TRUE(adapter != nullptr);

    PollEvent ev = adapter->PollNext(0);
    ASSERT_EQ(ev.kind, PollEventKind::kError);
    ASSERT_EQ(ev.err, -ENOTSUP);

    std::vector<std::string> unsupported;
    ASSERT_EQ(adapter->SetFilter("{}", &unsupported), ENOTSUP);
    ASSERT_TRUE(unsupported.empty());

    adapter->Cancel();
    ASSERT_EQ(adapter->Put(MakeBatch(1), 3001), ECANCELED);

    ASSERT_TRUE(!adapter->IsFinished());
    ASSERT_EQ(adapter->Close(), 0);
    ASSERT_TRUE(adapter->IsFinished());
}

void TestT21RingMpscConcurrency() {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 400;
    constexpr int kTotal = kProducers * kPerProducer;

    RingStreamChannelOptions opts;
    opts.ring_size = 1024;
    opts.ring_mode = RingMode::MPSC;
    opts.overflow = OverflowPolicy::kBlock;
    opts.finite = true;

    RingStreamChannel ch("ring", "t21_mpsc", opts);
    ASSERT_EQ(ch.Open(), 0);

    StreamChannelCapabilities caps = ch.Capabilities();
    ASSERT_EQ(caps.concurrency.put_mode, ProducerMode::MULTI);
    ASSERT_EQ(caps.concurrency.poll_mode, ConsumerMode::SINGLE);
    ASSERT_EQ(caps.concurrency.max_consumers, uint32_t(1));

    std::unordered_set<int64_t> seen;
    std::mutex seen_mu;
    std::atomic<int> consumed{0};

    std::thread consumer([&]() {
        while (true) {
            PollEvent ev = ch.PollNext(50);
            if (ev.kind == PollEventKind::kTimeout) continue;
            if (ev.kind == PollEventKind::kEof || ev.kind == PollEventKind::kDrainedAfterCancel) break;
            ASSERT_EQ(ev.kind, PollEventKind::kData);
            ASSERT_TRUE(ev.batch.data != nullptr);
            ASSERT_EQ(ev.batch.data->num_columns(), 1);
            auto arr = std::static_pointer_cast<arrow::Int64Array>(ev.batch.data->column(0));
            ASSERT_TRUE(arr != nullptr);
            for (int64_t i = 0; i < ev.batch.data->num_rows(); ++i) {
                const int64_t v = arr->Value(i);
                {
                    std::lock_guard<std::mutex> lock(seen_mu);
                    seen.insert(v);
                }
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&ch, p]() {
            for (int i = 0; i < kPerProducer; ++i) {
                const int64_t v = static_cast<int64_t>(p) * 1000000 + i;
                ASSERT_EQ(ch.Put(MakeBatch(v), v), 0);
            }
        });
    }
    for (auto& t : producers) t.join();

    ch.CloseStream();
    consumer.join();

    ASSERT_EQ(consumed.load(std::memory_order_relaxed), kTotal);
    ASSERT_EQ(static_cast<int>(seen.size()), kTotal);
    ASSERT_EQ(ch.Close(), 0);
}

void TestT22RingMpmcConcurrency() {
    constexpr int kProducers = 4;
    constexpr int kConsumers = 3;
    constexpr int kPerProducer = 400;
    constexpr int kTotal = kProducers * kPerProducer;

    RingStreamChannelOptions opts;
    opts.ring_size = 1024;
    opts.ring_mode = RingMode::MPMC;
    opts.overflow = OverflowPolicy::kBlock;
    opts.finite = true;

    RingStreamChannel ch("ring", "t22_mpmc", opts);
    ASSERT_EQ(ch.Open(), 0);

    StreamChannelCapabilities caps = ch.Capabilities();
    ASSERT_EQ(caps.concurrency.put_mode, ProducerMode::MULTI);
    ASSERT_EQ(caps.concurrency.poll_mode, ConsumerMode::MULTI);
    ASSERT_EQ(caps.concurrency.max_producers, uint32_t(0));
    ASSERT_EQ(caps.concurrency.max_consumers, uint32_t(0));

    std::unordered_set<int64_t> seen;
    std::mutex seen_mu;
    std::atomic<int> consumed{0};

    std::vector<std::thread> consumers;
    consumers.reserve(kConsumers);
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&]() {
            while (true) {
                PollEvent ev = ch.PollNext(50);
                if (ev.kind == PollEventKind::kTimeout) continue;
                if (ev.kind == PollEventKind::kEof || ev.kind == PollEventKind::kDrainedAfterCancel) break;
                ASSERT_EQ(ev.kind, PollEventKind::kData);
                ASSERT_TRUE(ev.batch.data != nullptr);
                auto arr = std::static_pointer_cast<arrow::Int64Array>(ev.batch.data->column(0));
                ASSERT_TRUE(arr != nullptr);
                for (int64_t i = 0; i < ev.batch.data->num_rows(); ++i) {
                    const int64_t v = arr->Value(i);
                    {
                        std::lock_guard<std::mutex> lock(seen_mu);
                        seen.insert(v);
                    }
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&ch, p]() {
            for (int i = 0; i < kPerProducer; ++i) {
                const int64_t v = static_cast<int64_t>(p) * 1000000 + i;
                ASSERT_EQ(ch.Put(MakeBatch(v), v), 0);
            }
        });
    }
    for (auto& t : producers) t.join();

    ch.CloseStream();
    for (auto& t : consumers) t.join();

    ASSERT_EQ(consumed.load(std::memory_order_relaxed), kTotal);
    ASSERT_EQ(static_cast<int>(seen.size()), kTotal);
    ASSERT_EQ(ch.Close(), 0);
}

void TestT23RuntimeRestartClearsStaleReadyQueue() {
    StreamRuntime runtime;
    runtime.Start(1);

    RingStreamChannelOptions opts;
    opts.ring_mode = RingMode::SPSC;
    opts.ring_size = 64;
    auto blocker_source = std::make_shared<RingStreamChannel>("ring", "t23_blocker_src", opts);
    ASSERT_EQ(blocker_source->Open(), 0);
    ASSERT_EQ(blocker_source->Put(MakeBatch(1), 1), 0);

    auto blocker_op = std::make_shared<MockStreamOperator>(ParallelStrategy::NONE, 1, 300);
    auto blocker_out = std::make_shared<DummyOutputChannel>();
    auto blocker_shard = std::make_shared<ShardRunner>(0, blocker_source, blocker_op, blocker_out, nullptr);
    ASSERT_TRUE(runtime.TrySchedule(blocker_shard));
    ASSERT_TRUE(WaitUntil([&]() { return blocker_op->IsInProcess(); }, 1000));

    auto stale_source = std::make_shared<RingStreamChannel>("ring", "t23_stale_src", opts);
    ASSERT_EQ(stale_source->Open(), 0);
    auto stale_op = std::make_shared<MockStreamOperator>(ParallelStrategy::NONE, 1);
    auto stale_out = std::make_shared<DummyOutputChannel>();
    auto stale_shard = std::make_shared<ShardRunner>(0, stale_source, stale_op, stale_out, nullptr);
    ASSERT_TRUE(runtime.TrySchedule(stale_shard));

    runtime.Stop();
    ASSERT_EQ(stale_op->ProcessCalls(), uint64_t(0));
    ASSERT_EQ(stale_op->TickCalls(), uint64_t(0));

    runtime.Start(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    ASSERT_EQ(stale_op->ProcessCalls(), uint64_t(0));
    ASSERT_EQ(stale_op->TickCalls(), uint64_t(0));

    blocker_source->Cancel();
    stale_source->Cancel();
    blocker_source->Close();
    stale_source->Close();
    runtime.Stop();
}

void TestT24FanOutOpenFailureRollback() {
    RingStreamChannelOptions source_opts;
    source_opts.ring_size = 64;
    source_opts.ring_mode = RingMode::SPSC;
    source_opts.overflow = OverflowPolicy::kDrop;
    auto source = std::make_shared<RingStreamChannel>("ring", "t24_source", source_opts);
    ASSERT_TRUE(!source->IsOpened());

    RingStreamChannelOptions bad_partition_opts;
    bad_partition_opts.ring_size = 3;  // 非 2 次幂，Open 必须失败
    bad_partition_opts.ring_mode = RingMode::SPSC;
    bad_partition_opts.overflow = OverflowPolicy::kDrop;

    FanOutStreamChannel fanout("fanout", "t24_fanout", source, 3, FanOutMode::ROUND_ROBIN, "", bad_partition_opts);
    const int rc = fanout.Open();
    ASSERT_TRUE(rc != 0);
    ASSERT_TRUE(!fanout.IsOpened());
    ASSERT_TRUE(!source->IsOpened());
    for (size_t i = 0; i < 3; ++i) {
        auto p = fanout.GetPartition(i);
        ASSERT_TRUE(p != nullptr);
        ASSERT_TRUE(!p->IsOpened());
    }
}

void TestT25StreamHubOpenFailureRollback() {
    StreamHubOptions opts;
    opts.mode = StreamHubMode::kSplit;
    opts.partition_count = 4;
    opts.partition_ring_mode = RingMode::SPSC;
    opts.partition_ring_size = 3;  // 非 2 次幂，分区 Open 失败

    StreamHubChannel hub("stream_hub", "t25_hub", opts);
    const int rc = hub.Open();
    ASSERT_TRUE(rc != 0);
    ASSERT_TRUE(!hub.IsOpened());
    ASSERT_EQ(hub.Put(MakeBatch(1), 1), EBADF);

    auto root_ev = hub.PollNext(0);
    ASSERT_EQ(root_ev.kind, PollEventKind::kError);
    ASSERT_EQ(root_ev.err, -ENOTSUP);
    for (size_t i = 0; i < hub.PartitionCount(); ++i) {
        auto part = hub.GetPartition(i);
        ASSERT_TRUE(part != nullptr);
        ASSERT_TRUE(!part->IsOpened());
    }
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("=== Stream Runtime Tests ===");

    std::puts("[TEST] T1 RingStreamChannel 基础读写");
    TestT1RingReadWrite();
    std::puts("[PASS] T1");

    std::puts("[TEST] T2 背压策略");
    TestT2Backpressure();
    std::puts("[PASS] T2");

    std::puts("[TEST] T3 EOF 传播");
    TestT3EofPropagation();
    std::puts("[PASS] T3");

    std::puts("[TEST] T4 FanIn 合并");
    TestT4FanInMerge();
    std::puts("[PASS] T4");

    std::puts("[TEST] T5 NONE 场景");
    TestT5NoneScenario();
    std::puts("[PASS] T5");

    std::puts("[TEST] T6 STATELESS 场景");
    TestT6StatelessScenarioSpmc();
    std::puts("[PASS] T6");

    std::puts("[TEST] T7 STATELESS 终止协议");
    TestT7StatelessTermination();
    std::puts("[PASS] T7");

    std::puts("[TEST] T8 KEYED 场景");
    TestT8KeyedScenario();
    std::puts("[PASS] T8");

    std::puts("[TEST] T9 RunnerStep 预算让出");
    TestT9RunnerStepBudgetYield();
    std::puts("[PASS] T9");

    std::puts("[TEST] T10 StreamTask 生命周期");
    TestT10StreamTaskLifecycleCancel();
    std::puts("[PASS] T10");

    std::puts("[TEST] T11 Cancel 超时兜底");
    TestT11CancelTimeoutFallback();
    std::puts("[PASS] T11");

    std::puts("[TEST] T12 动态 schema 空流");
    TestT12DynamicSchemaEmptyStream();
    std::puts("[PASS] T12");

    std::puts("[TEST] T13 TaskSnapshot 聚合一致性");
    TestT13TaskSnapshotAggregation();
    std::puts("[PASS] T13");

    std::puts("[TEST] T14 状态机迁移完整性");
    TestT14StateMachineTransitions();
    std::puts("[PASS] T14");

    std::puts("[TEST] T15 kDone 吸收性");
    TestT15DoneAbsorption();
    std::puts("[PASS] T15");

    std::puts("[TEST] T16 TcpSessionMock 三模式");
    TestT16TcpSessionMockThreeModes();
    std::puts("[PASS] T16");

    std::puts("[TEST] T17 TcpServiceMerge 聚合");
    TestT17TcpServiceMergeOperator();
    std::puts("[PASS] T17");

    std::puts("[TEST] T18 StreamChannelAdapter -> DataFrame Append");
    TestT18StreamChannelAdapterDataFrameAppend();
    std::puts("[PASS] T18");

    std::puts("[TEST] T19 StreamChannelAdapter -> Database Writer");
    TestT19StreamChannelAdapterDatabaseWriter();
    std::puts("[PASS] T19");

    std::puts("[TEST] T20 StreamChannelAdapter 输出侧契约");
    TestT20StreamChannelAdapterOutputOnlyContract();
    std::puts("[PASS] T20");

    std::puts("[TEST] T21 Ring MPSC 并发正确性");
    TestT21RingMpscConcurrency();
    std::puts("[PASS] T21");

    std::puts("[TEST] T22 Ring MPMC 并发正确性");
    TestT22RingMpmcConcurrency();
    std::puts("[PASS] T22");

    std::puts("[TEST] T23 StreamRuntime Stop->Start 队列清理");
    TestT23RuntimeRestartClearsStaleReadyQueue();
    std::puts("[PASS] T23");

    std::puts("[TEST] T24 FanOut Open 失败回滚");
    TestT24FanOutOpenFailureRollback();
    std::puts("[PASS] T24");

    std::puts("[TEST] T25 StreamHub Open 失败回滚");
    TestT25StreamHubOpenFailureRollback();
    std::puts("[PASS] T25");

    std::puts("=== All stream tests passed ===");
    return 0;
}
