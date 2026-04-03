#ifndef _FLOWSQL_FRAMEWORK_CORE_RING_STREAM_CHANNEL_H_
#define _FLOWSQL_FRAMEWORK_CORE_RING_STREAM_CHANNEL_H_

#include <framework/interfaces/istream_channel.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace flowsql {

enum class RingMode {
    SPSC,
    MPSC,
    SPMC,
    MPMC,
};

enum class OverflowPolicy {
    kDrop,
    kBlock,
};

struct RingStreamChannelOptions {
    size_t ring_size = 64;  // 必须是 2 的幂
    int batch_rows = 1024;
    OverflowPolicy overflow = OverflowPolicy::kDrop;
    RingMode ring_mode = RingMode::SPSC;
    bool finite = false;
    std::shared_ptr<arrow::Schema> static_schema;
};

class IRing {
 public:
    virtual ~IRing() = default;
    virtual int enqueue(StreamBatch batch) = 0;
    virtual int dequeue(StreamBatch* out) = 0;  // 非阻塞，空则返回 ETIMEDOUT
    virtual size_t size() const = 0;
    virtual size_t capacity() const = 0;
};

// AtomicRing — C++17 原子实现，当前支持 SPSC/SPMC。
class AtomicRing : public IRing {
 public:
    AtomicRing(size_t capacity, RingMode mode);
    ~AtomicRing() override = default;

    int enqueue(StreamBatch batch) override;
    int dequeue(StreamBatch* out) override;
    size_t size() const override;
    size_t capacity() const override;
    bool mode_supported() const { return mode_supported_; }
    bool valid_capacity() const { return valid_capacity_; }

 private:
    struct Slot {
        std::atomic<size_t> seq{0};
        StreamBatch batch{};
    };

    int EnqueueSingleProducer(StreamBatch batch);
    int DequeueSingleConsumer(StreamBatch* out);
    int DequeueMultiConsumer(StreamBatch* out);

    std::unique_ptr<Slot[]> slots_;
    const size_t capacity_;
    const size_t mask_;
    const RingMode mode_;
    const bool valid_capacity_;
    const bool mode_supported_;

    alignas(64) std::atomic<size_t> head_{0};  // 生产者写位置
    alignas(64) std::atomic<size_t> tail_{0};  // 消费者读位置
};

class RingStreamChannel : public IStreamChannel {
 public:
    RingStreamChannel(std::string category,
                      std::string name,
                      const RingStreamChannelOptions& options = {});
    ~RingStreamChannel() override = default;

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

    bool IsFull() const override;
    bool IsEmpty() const override;
    size_t Capacity() const override;
    size_t Size() const override;
    bool IsFinite() const override { return options_.finite; }

    void CloseStream() override;
    void Cancel() override;
    bool IsFinished() const override { return finished_.load(std::memory_order_acquire); }

 private:
    int EnqueueWithPolicy(StreamBatch batch, bool block);
    int PushEofWithRetry(int max_wait_ms);

    std::string category_;
    std::string name_;
    RingStreamChannelOptions options_;
    std::unique_ptr<IRing> ring_;

    std::string schema_cache_ = "[]";

    std::atomic<bool> opened_{false};
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> finished_{false};
    int open_error_ = 0;
    std::once_flag cancel_once_;
    std::once_flag close_stream_once_;

    std::condition_variable cv_;
    mutable std::mutex cv_mu_;
    std::atomic<uint64_t> dropped_batches_{0};
};

RingMode ParseRingMode(const std::string& mode);
OverflowPolicy ParseOverflowPolicy(const std::string& value);

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_RING_STREAM_CHANNEL_H_
