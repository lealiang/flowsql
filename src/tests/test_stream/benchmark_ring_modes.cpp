#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>
#include <ctime>

#include <arrow/api.h>

#include <framework/core/ring_stream_channel.h>

using namespace flowsql;

namespace {

struct Scenario {
    const char* name;
    RingMode mode;
    int producers;
    int consumers;
};

struct Metrics {
    int64_t total_messages = 0;
    int64_t consumed_messages = 0;
    int64_t duplicate_messages = 0;
    int64_t missing_messages = 0;
    int64_t wall_time_ms = 0;
    double throughput_rows_per_sec = 0.0;
    int64_t p50_latency_us = 0;
    int64_t p99_latency_us = 0;
    int64_t max_latency_us = 0;
    double cpu_percent = 0.0;
};

static std::shared_ptr<arrow::RecordBatch> MakeBatch(int64_t value) {
    auto schema = arrow::schema({arrow::field("id", arrow::int64())});
    arrow::Int64Builder builder;
    (void)builder.Append(value);
    auto arr = builder.Finish().ValueOrDie();
    return arrow::RecordBatch::Make(schema, 1, {arr});
}

static int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static int64_t Percentile(std::vector<int64_t>* values, double p) {
    if (!values || values->empty()) return 0;
    const size_t n = values->size();
    const size_t idx = static_cast<size_t>(std::floor((n - 1) * p));
    std::nth_element(values->begin(), values->begin() + idx, values->end());
    return (*values)[idx];
}

static Metrics RunScenario(const Scenario& s, int per_producer) {
    Metrics m;
    m.total_messages = static_cast<int64_t>(s.producers) * per_producer;

    RingStreamChannelOptions opts;
    opts.ring_mode = s.mode;
    opts.overflow = OverflowPolicy::kBlock;
    opts.finite = true;
    opts.ring_size = 4096;

    RingStreamChannel channel("ring", s.name, opts);
    if (channel.Open() != 0) {
        std::printf("[FAIL] open channel failed for %s\n", s.name);
        return m;
    }

    std::vector<std::atomic<uint8_t>> seen(static_cast<size_t>(m.total_messages));
    std::vector<std::atomic<int64_t>> produced_ns(static_cast<size_t>(m.total_messages));
    for (auto& x : seen) x.store(0, std::memory_order_relaxed);
    for (auto& x : produced_ns) x.store(0, std::memory_order_relaxed);

    std::vector<int64_t> latencies_us;
    latencies_us.reserve(static_cast<size_t>(m.total_messages));
    std::mutex lat_mu;
    std::atomic<int64_t> consumed{0};
    std::atomic<int64_t> dup{0};

    const std::clock_t cpu_start = std::clock();
    const auto wall_start = std::chrono::steady_clock::now();

    std::vector<std::thread> consumers;
    consumers.reserve(static_cast<size_t>(s.consumers));
    for (int c = 0; c < s.consumers; ++c) {
        consumers.emplace_back([&]() {
            while (true) {
                PollEvent ev = channel.PollNext(20);
                if (ev.kind == PollEventKind::kTimeout) continue;
                if (ev.kind == PollEventKind::kEof || ev.kind == PollEventKind::kDrainedAfterCancel) break;
                if (ev.kind != PollEventKind::kData || !ev.batch.data) continue;

                auto arr = std::static_pointer_cast<arrow::Int64Array>(ev.batch.data->column(0));
                if (!arr || ev.batch.data->num_rows() <= 0) continue;
                const int64_t id = arr->Value(0);
                if (id < 0 || id >= m.total_messages) continue;

                uint8_t expected = 0;
                if (!seen[static_cast<size_t>(id)].compare_exchange_strong(
                        expected, static_cast<uint8_t>(1), std::memory_order_acq_rel)) {
                    dup.fetch_add(1, std::memory_order_relaxed);
                }

                const int64_t t0 = produced_ns[static_cast<size_t>(id)].load(std::memory_order_acquire);
                if (t0 > 0) {
                    const int64_t latency = (NowNs() - t0) / 1000;
                    std::lock_guard<std::mutex> lock(lat_mu);
                    latencies_us.push_back(latency);
                }
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::vector<std::thread> producers;
    producers.reserve(static_cast<size_t>(s.producers));
    for (int p = 0; p < s.producers; ++p) {
        producers.emplace_back([&, p]() {
            const int64_t base = static_cast<int64_t>(p) * per_producer;
            for (int i = 0; i < per_producer; ++i) {
                const int64_t id = base + i;
                produced_ns[static_cast<size_t>(id)].store(NowNs(), std::memory_order_release);
                const int rc = channel.Put(MakeBatch(id), id);
                if (rc != 0) {
                    std::printf("[WARN] put failed: scenario=%s rc=%d id=%lld\n",
                                s.name, rc, static_cast<long long>(id));
                }
            }
        });
    }

    for (auto& t : producers) t.join();
    channel.CloseStream();
    for (auto& t : consumers) t.join();

    const auto wall_end = std::chrono::steady_clock::now();
    const std::clock_t cpu_end = std::clock();
    (void)channel.Close();

    m.consumed_messages = consumed.load(std::memory_order_relaxed);
    m.duplicate_messages = dup.load(std::memory_order_relaxed);
    m.missing_messages = m.total_messages - m.consumed_messages;
    if (m.missing_messages < 0) m.missing_messages = 0;

    m.wall_time_ms = std::max<int64_t>(
        1,
        std::chrono::duration_cast<std::chrono::milliseconds>(wall_end - wall_start).count());
    m.throughput_rows_per_sec = static_cast<double>(m.consumed_messages) * 1000.0 /
                                static_cast<double>(m.wall_time_ms);

    if (!latencies_us.empty()) {
        m.p50_latency_us = Percentile(&latencies_us, 0.50);
        m.p99_latency_us = Percentile(&latencies_us, 0.99);
        m.max_latency_us = *std::max_element(latencies_us.begin(), latencies_us.end());
    }

    const double cpu_ms = static_cast<double>(cpu_end - cpu_start) * 1000.0 / CLOCKS_PER_SEC;
    m.cpu_percent = cpu_ms * 100.0 / static_cast<double>(m.wall_time_ms);
    return m;
}

}  // namespace

int main() {
    constexpr int kPerProducer = 20000;
    const std::vector<Scenario> scenarios = {
        {"spsc", RingMode::SPSC, 1, 1},
        {"spmc", RingMode::SPMC, 1, 4},
        {"mpsc", RingMode::MPSC, 4, 1},
        {"mpmc", RingMode::MPMC, 4, 4},
    };

    std::puts("mode,total,consumed,duplicate,missing,wall_ms,throughput_rows_s,p50_us,p99_us,max_us,cpu_percent");
    for (const auto& s : scenarios) {
        Metrics m = RunScenario(s, kPerProducer);
        std::printf("%s,%lld,%lld,%lld,%lld,%lld,%.2f,%lld,%lld,%lld,%.2f\n",
                    s.name,
                    static_cast<long long>(m.total_messages),
                    static_cast<long long>(m.consumed_messages),
                    static_cast<long long>(m.duplicate_messages),
                    static_cast<long long>(m.missing_messages),
                    static_cast<long long>(m.wall_time_ms),
                    m.throughput_rows_per_sec,
                    static_cast<long long>(m.p50_latency_us),
                    static_cast<long long>(m.p99_latency_us),
                    static_cast<long long>(m.max_latency_us),
                    m.cpu_percent);
    }
    return 0;
}
