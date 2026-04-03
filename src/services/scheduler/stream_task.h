#ifndef _FLOWSQL_SERVICES_SCHEDULER_STREAM_TASK_H_
#define _FLOWSQL_SERVICES_SCHEDULER_STREAM_TASK_H_

#include <framework/interfaces/ichannel.h>
#include <framework/interfaces/istream_channel.h>
#include <framework/interfaces/istream_operator.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace flowsql {
namespace scheduler {

class StreamRuntime;

enum class StreamTaskStatus {
    kCreated,
    kRunning,
    kStopping,
    kStopped,
    kCancelled,
    kFailed,
};

enum class ShardExecState {
    kIdle,
    kQueued,
    kRunning,
    kRunningPending,
    kWaitingRetry,
    kDone,
};

struct TaskMetrics {
    std::atomic<uint64_t> processed_batches{0};
    std::atomic<uint64_t> processed_rows{0};
    std::atomic<uint64_t> processed_bytes{0};
    std::atomic<uint64_t> output_rows{0};
    std::atomic<uint64_t> output_batches{0};
    std::atomic<uint64_t> dropped_batches{0};
    std::atomic<uint64_t> poll_timeouts{0};
    std::atomic<uint64_t> poll_errors{0};
    std::atomic<uint64_t> queue_depth_peak{0};
};

struct ErrorInfo {
    int code = 0;
    std::string message;
};

struct TaskSnapshot {
    std::string task_id;
    StreamTaskStatus status = StreamTaskStatus::kCreated;
    bool stop_requested = false;
    bool joined = false;
    uint32_t shard_count = 0;
    uint32_t active_shards = 0;

    uint64_t processed_batches = 0;
    uint64_t processed_rows = 0;
    uint64_t processed_bytes = 0;
    uint64_t output_rows = 0;
    uint64_t output_batches = 0;
    uint64_t dropped_batches = 0;
    uint64_t poll_timeouts = 0;
    uint64_t poll_errors = 0;
    uint64_t queue_depth = 0;
    uint64_t queue_depth_peak = 0;
    int64_t uptime_ms = 0;

    int64_t started_ms = 0;
    int64_t last_active_ms = 0;
    int64_t finished_ms = 0;

    int error_code = 0;
    std::string error_message;
    std::string op_stats_json;
};

enum StepResult {
    kStepDone = 0,
    kStepYield = 1,
    kStepNeedRetryLater = 2,
};

class StreamTask;

class ShardRunner final {
 public:
    ShardRunner(uint32_t shard_id,
                std::shared_ptr<IStreamChannel> input,
                std::shared_ptr<IStreamOperator> op,
                std::shared_ptr<IChannel> output,
                StreamTask* owner,
                bool schema_ready = false);

    int Step();
    int Finalize();
    void RequestStop();
    void MarkDone();
    size_t QueueDepth() const;
    std::string OpStatsJson() const;

 public:
    std::atomic<ShardExecState> exec_state{ShardExecState::kIdle};
    std::atomic<bool> stop_requested{false};
    std::once_flag stop_once;
    TaskMetrics metrics;

 private:
    uint32_t shard_id_ = 0;
    std::shared_ptr<IStreamChannel> input_;
    std::shared_ptr<IStreamOperator> op_;
    std::shared_ptr<IChannel> output_;
    StreamTask* owner_ = nullptr;  // non-owning
    bool schema_ready_ = false;
    std::atomic<bool> finalized_{false};
    std::atomic<int64_t> started_ms_{0};
    std::atomic<int64_t> last_active_ms_{0};
    std::atomic<int64_t> finished_ms_{0};
};

class StreamTask final {
 public:
    StreamTask(std::string task_id, StreamRuntime* runtime);

    const std::string& Id() const { return task_id_; }
    void PrepareForRun(uint32_t shard_count, int64_t start_ms);
    void AddShard(const std::shared_ptr<ShardRunner>& shard);
    const std::vector<std::shared_ptr<ShardRunner>>& Shards() const { return shards_; }

    void RequestStop();
    void Join();
    TaskSnapshot Snapshot() const;
    void SetFailedOnce(int code, const std::string& msg);
    void OnShardDone();
    void TouchActive(int64_t now_ms);

    StreamTaskStatus Status() const {
        return status_.load(std::memory_order_acquire);
    }

 private:
    std::string task_id_;
    std::atomic<StreamTaskStatus> status_{StreamTaskStatus::kCreated};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> joined_{false};
    std::atomic<uint32_t> active_shards_{0};
    std::vector<std::shared_ptr<ShardRunner>> shards_;

    std::once_flag stop_once_;
    mutable std::mutex done_mu_;
    mutable std::condition_variable done_cv_;
    std::shared_ptr<const ErrorInfo> error_;  // first-failure-wins
    StreamRuntime* runtime_ = nullptr;

    std::atomic<int64_t> started_ms_{0};
    std::atomic<int64_t> last_active_ms_{0};
    std::atomic<int64_t> finished_ms_{0};
};

}  // namespace scheduler
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_SCHEDULER_STREAM_TASK_H_
