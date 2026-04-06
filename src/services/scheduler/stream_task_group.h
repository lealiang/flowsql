#ifndef _FLOWSQL_SERVICES_SCHEDULER_STREAM_TASK_GROUP_H_
#define _FLOWSQL_SERVICES_SCHEDULER_STREAM_TASK_GROUP_H_

#include "stream_task.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace flowsql {
namespace scheduler {

enum class GroupStartCondition {
    kOnRunning,
    kOnFinished,
};

enum class GroupNodeStatus {
    kPending,
    kReady,
    kRunning,
    kStopping,
    kStopped,
    kCancelled,
    kFailed,
    kSkipped,
};

enum class StreamGroupStatus {
    kCreated,
    kPreparing,
    kRunning,
    kStopping,
    kStopped,
    kCancelled,
    kFailed,
};

struct GroupNodePlan {
    std::string id;
    std::string sql;
    std::vector<std::string> depends_on;
    GroupStartCondition start_condition = GroupStartCondition::kOnRunning;
};

struct GroupNodeSnapshot {
    std::string node_id;
    std::string runtime_task_id;
    GroupNodeStatus status = GroupNodeStatus::kPending;
    GroupStartCondition start_condition = GroupStartCondition::kOnRunning;
    std::vector<std::string> depends_on;

    std::string error_code;
    int error_no = 0;
    std::string error_message;

    uint64_t processed_rows = 0;
    uint64_t output_rows = 0;
    uint64_t dropped_batches = 0;
    uint64_t poll_errors = 0;
    int64_t started_ms = 0;
    int64_t last_active_ms = 0;
    int64_t finished_ms = 0;
};

struct StreamGroupSnapshot {
    std::string task_id;
    std::string runtime_task_id;
    StreamGroupStatus status = StreamGroupStatus::kCreated;
    std::string group_mode = "dag";
    bool stop_requested = false;

    uint32_t node_count = 0;
    uint32_t active_nodes = 0;
    std::string error_code;
    int error_no = 0;
    std::string error_message;

    int64_t started_ms = 0;
    int64_t last_active_ms = 0;
    int64_t finished_ms = 0;
    std::vector<GroupNodeSnapshot> nodes;
};

const char* GroupStartConditionName(GroupStartCondition cond);
const char* GroupNodeStatusName(GroupNodeStatus status);
const char* StreamGroupStatusName(StreamGroupStatus status);
bool IsTerminalStreamGroupStatus(StreamGroupStatus status);

class StreamTaskGroup final {
 public:
    using SubmitNodeFn = std::function<int(const std::string& node_id,
                                           const std::string& sql,
                                           std::string* runtime_task_id,
                                           std::string* error_msg)>;
    using QueryNodeFn = std::function<int(const std::string& runtime_task_id,
                                          TaskSnapshot* snapshot_out)>;
    using StopNodeFn = std::function<void(const std::string& runtime_task_id)>;
    using PreStopFn = std::function<void()>;

    StreamTaskGroup(std::string task_id,
                    std::string runtime_task_id,
                    std::vector<GroupNodePlan> nodes,
                    int timeout_s,
                    SubmitNodeFn submit_fn,
                    QueryNodeFn query_fn,
                    StopNodeFn stop_fn,
                    PreStopFn pre_stop_fn = nullptr);
    ~StreamTaskGroup();

    int Start(std::string* err_msg);
    void RequestStop(bool cancelled = false);
    void MarkExternalFailed(int code, const std::string& message, const std::string& error_code = "");
    void Join();
    StreamGroupSnapshot Snapshot() const;
    bool IsTerminal() const;

 private:
    struct NodeState {
        GroupNodePlan plan;
        GroupNodeStatus status = GroupNodeStatus::kPending;
        std::string runtime_task_id;
        bool submitted = false;
        bool stop_sent = false;
        std::string error_code;
        int error_no = 0;
        std::string error_message;
        TaskSnapshot task_snapshot;
    };

    void RunLoop();
    bool TrySubmitReadyNodes(int64_t now_ms);
    void RefreshNodeStates();
    bool HandleFailureOrTimeout(int64_t now_ms);
    void HandleStopSignal();
    bool DependenciesSatisfied(const NodeState& node) const;
    bool IsNodeTerminal(GroupNodeStatus status) const;
    bool IsNodeStarted(GroupNodeStatus status) const;
    bool AllNodesTerminal() const;
    void MarkGroupFailed(int code, const std::string& message, int64_t now_ms, const std::string& error_code = "");
    void TryFinalize(int64_t now_ms);
    void Touch(int64_t now_ms);

 private:
    std::string task_id_;
    std::string runtime_task_id_;
    int timeout_s_ = 0;
    SubmitNodeFn submit_fn_;
    QueryNodeFn query_fn_;
    StopNodeFn stop_fn_;
    PreStopFn pre_stop_fn_;

    std::unordered_map<std::string, size_t> node_index_by_id_;
    std::vector<NodeState> nodes_;

    mutable std::mutex mu_;
    std::condition_variable done_cv_;
    std::thread runner_;
    bool started_ = false;

    std::atomic<StreamGroupStatus> status_{StreamGroupStatus::kCreated};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> cancel_requested_{false};

    std::string error_code_;
    int error_no_ = 0;
    std::string error_message_;

    bool pre_stop_invoked_ = false;

    int64_t started_ms_ = 0;
    int64_t last_active_ms_ = 0;
    int64_t finished_ms_ = 0;
};

}  // namespace scheduler
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_SCHEDULER_STREAM_TASK_GROUP_H_
