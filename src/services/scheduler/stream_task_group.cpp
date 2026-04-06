#include "stream_task_group.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <sstream>

namespace flowsql {
namespace scheduler {

namespace {

int64_t CurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

GroupNodeStatus MapTaskStatus(StreamTaskStatus status) {
    switch (status) {
        case StreamTaskStatus::kCreated: return GroupNodeStatus::kReady;
        case StreamTaskStatus::kRunning: return GroupNodeStatus::kRunning;
        case StreamTaskStatus::kStopping: return GroupNodeStatus::kStopping;
        case StreamTaskStatus::kStopped: return GroupNodeStatus::kStopped;
        case StreamTaskStatus::kCancelled: return GroupNodeStatus::kCancelled;
        case StreamTaskStatus::kFailed: return GroupNodeStatus::kFailed;
        default: return GroupNodeStatus::kFailed;
    }
}

}  // namespace

const char* GroupStartConditionName(GroupStartCondition cond) {
    return cond == GroupStartCondition::kOnFinished ? "on_finished" : "on_running";
}

const char* GroupNodeStatusName(GroupNodeStatus status) {
    switch (status) {
        case GroupNodeStatus::kPending: return "pending";
        case GroupNodeStatus::kReady: return "ready";
        case GroupNodeStatus::kRunning: return "running";
        case GroupNodeStatus::kStopping: return "stopping";
        case GroupNodeStatus::kStopped: return "stopped";
        case GroupNodeStatus::kCancelled: return "cancelled";
        case GroupNodeStatus::kFailed: return "failed";
        case GroupNodeStatus::kSkipped: return "skipped";
        default: return "unknown";
    }
}

const char* StreamGroupStatusName(StreamGroupStatus status) {
    switch (status) {
        case StreamGroupStatus::kCreated: return "created";
        case StreamGroupStatus::kPreparing: return "preparing";
        case StreamGroupStatus::kRunning: return "running";
        case StreamGroupStatus::kStopping: return "stopping";
        case StreamGroupStatus::kStopped: return "stopped";
        case StreamGroupStatus::kCancelled: return "cancelled";
        case StreamGroupStatus::kFailed: return "failed";
        default: return "unknown";
    }
}

bool IsTerminalStreamGroupStatus(StreamGroupStatus status) {
    return status == StreamGroupStatus::kStopped ||
           status == StreamGroupStatus::kCancelled ||
           status == StreamGroupStatus::kFailed;
}

StreamTaskGroup::StreamTaskGroup(std::string task_id,
                                 std::string runtime_task_id,
                                 std::vector<GroupNodePlan> nodes,
                                 int timeout_s,
                                 SubmitNodeFn submit_fn,
                                 QueryNodeFn query_fn,
                                 StopNodeFn stop_fn,
                                 PreStopFn pre_stop_fn)
    : task_id_(std::move(task_id)),
      runtime_task_id_(std::move(runtime_task_id)),
      timeout_s_(timeout_s),
      submit_fn_(std::move(submit_fn)),
      query_fn_(std::move(query_fn)),
      stop_fn_(std::move(stop_fn)),
      pre_stop_fn_(std::move(pre_stop_fn)) {
    nodes_.reserve(nodes.size());
    for (auto& plan : nodes) {
        node_index_by_id_[plan.id] = nodes_.size();
        NodeState state;
        state.plan = std::move(plan);
        nodes_.push_back(std::move(state));
    }
}

StreamTaskGroup::~StreamTaskGroup() {
    RequestStop();
    Join();
}

int StreamTaskGroup::Start(std::string* err_msg) {
    std::lock_guard<std::mutex> lock(mu_);
    if (started_) return 0;
    if (!submit_fn_ || !query_fn_ || !stop_fn_) {
        if (err_msg) *err_msg = "group callbacks are not ready";
        return EINVAL;
    }
    started_ = true;
    started_ms_ = CurrentTimeMs();
    last_active_ms_ = started_ms_;
    status_.store(StreamGroupStatus::kPreparing, std::memory_order_release);
    runner_ = std::thread(&StreamTaskGroup::RunLoop, this);
    return 0;
}

void StreamTaskGroup::RequestStop(bool cancelled) {
    stop_requested_.store(true, std::memory_order_release);
    if (cancelled) {
        cancel_requested_.store(true, std::memory_order_release);
    }
}

void StreamTaskGroup::MarkExternalFailed(int code,
                                         const std::string& message,
                                         const std::string& error_code) {
    std::lock_guard<std::mutex> lock(mu_);
    MarkGroupFailed(code != 0 ? code : EIO,
                    message,
                    CurrentTimeMs(),
                    error_code.empty() ? "STREAM_GROUP_EXTERNAL_FAILED" : error_code);
}

void StreamTaskGroup::Join() {
    std::thread local_runner;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (runner_.joinable()) {
            local_runner = std::move(runner_);
        }
    }
    if (local_runner.joinable()) {
        local_runner.join();
    }
}

bool StreamTaskGroup::IsTerminal() const {
    return IsTerminalStreamGroupStatus(status_.load(std::memory_order_acquire));
}

StreamGroupSnapshot StreamTaskGroup::Snapshot() const {
    StreamGroupSnapshot s;
    s.task_id = task_id_;
    s.runtime_task_id = runtime_task_id_;
    s.status = status_.load(std::memory_order_acquire);
    s.group_mode = "dag";
    s.stop_requested = stop_requested_.load(std::memory_order_acquire);
    s.error_code = error_code_;
    s.error_no = error_no_;
    s.error_message = error_message_;
    s.started_ms = started_ms_;
    s.last_active_ms = last_active_ms_;
    s.finished_ms = finished_ms_;

    std::lock_guard<std::mutex> lock(mu_);
    s.node_count = static_cast<uint32_t>(nodes_.size());
    s.active_nodes = 0;
    s.nodes.reserve(nodes_.size());
    for (const auto& node : nodes_) {
        if (!IsNodeTerminal(node.status)) {
            s.active_nodes += 1;
        }
        GroupNodeSnapshot n;
        n.node_id = node.plan.id;
        n.runtime_task_id = node.runtime_task_id;
        n.status = node.status;
        n.start_condition = node.plan.start_condition;
        n.depends_on = node.plan.depends_on;
        n.error_code = node.error_code;
        n.error_no = node.error_no;
        n.error_message = node.error_message;
        n.processed_rows = node.task_snapshot.processed_rows;
        n.output_rows = node.task_snapshot.output_rows;
        n.dropped_batches = node.task_snapshot.dropped_batches;
        n.poll_errors = node.task_snapshot.poll_errors;
        n.started_ms = node.task_snapshot.started_ms;
        n.last_active_ms = node.task_snapshot.last_active_ms;
        n.finished_ms = node.task_snapshot.finished_ms;
        s.nodes.push_back(std::move(n));
    }
    return s;
}

void StreamTaskGroup::RunLoop() {
    while (true) {
        const int64_t now_ms = CurrentTimeMs();
        RefreshNodeStates();
        if (HandleFailureOrTimeout(now_ms)) {
            Touch(now_ms);
        }
        if (stop_requested_.load(std::memory_order_acquire)) {
            HandleStopSignal();
            if (status_.load(std::memory_order_acquire) != StreamGroupStatus::kFailed) {
                status_.store(StreamGroupStatus::kStopping, std::memory_order_release);
            }
        } else if (status_.load(std::memory_order_acquire) == StreamGroupStatus::kPreparing) {
            status_.store(StreamGroupStatus::kRunning, std::memory_order_release);
        }

        if (status_.load(std::memory_order_acquire) != StreamGroupStatus::kStopping &&
            status_.load(std::memory_order_acquire) != StreamGroupStatus::kFailed) {
            if (TrySubmitReadyNodes(now_ms)) {
                Touch(now_ms);
            }
        }

        if (AllNodesTerminal()) {
            TryFinalize(now_ms);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool StreamTaskGroup::TrySubmitReadyNodes(int64_t now_ms) {
    struct SubmitAction {
        size_t index = 0;
        uint64_t generation = 0;
        std::string node_id;
        std::string sql;
    };
    struct SubmitResult {
        size_t index = 0;
        uint64_t generation = 0;
        int rc = EIO;
        std::string runtime_task_id;
        std::string err_msg;
    };

    std::vector<SubmitAction> actions;
    {
        std::lock_guard<std::mutex> lock(mu_);
        actions.reserve(nodes_.size());
        for (size_t i = 0; i < nodes_.size(); ++i) {
            auto& node = nodes_[i];
            if (node.submitted || IsNodeTerminal(node.status) || node.submit_inflight) continue;
            if (!DependenciesSatisfied(node)) continue;

            node.status = GroupNodeStatus::kReady;
            node.submit_inflight = true;
            node.generation += 1;
            SubmitAction action;
            action.index = i;
            action.generation = node.generation;
            action.node_id = node.plan.id;
            action.sql = node.plan.sql;
            actions.push_back(std::move(action));
        }
    }
    if (actions.empty()) return false;

    std::vector<SubmitResult> results;
    results.reserve(actions.size());
    for (const auto& action : actions) {
        SubmitResult result;
        result.index = action.index;
        result.generation = action.generation;
        result.rc = submit_fn_(action.node_id, action.sql, &result.runtime_task_id, &result.err_msg);
        results.push_back(std::move(result));
    }

    bool submitted_any = false;
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& result : results) {
        if (result.index >= nodes_.size()) continue;
        auto& node = nodes_[result.index];
        if (!node.submit_inflight) continue;

        node.submit_inflight = false;
        if (node.generation != result.generation) {
            continue;
        }
        if (result.rc != 0 || result.runtime_task_id.empty()) {
            node.status = GroupNodeStatus::kFailed;
            node.error_no = result.rc != 0 ? result.rc : EIO;
            node.error_code = "STREAM_GROUP_NODE_SUBMIT_FAILED";
            node.error_message = result.err_msg.empty() ? "submit group node failed" : result.err_msg;
            MarkGroupFailed(node.error_no,
                            "group node submit failed: " + node.plan.id,
                            now_ms,
                            "STREAM_GROUP_NODE_SUBMIT_FAILED");
            continue;
        }
        node.runtime_task_id = result.runtime_task_id;
        node.submitted = true;
        node.status = GroupNodeStatus::kRunning;
        submitted_any = true;
    }
    return submitted_any;
}

void StreamTaskGroup::RefreshNodeStates() {
    struct QueryAction {
        size_t index = 0;
        uint64_t generation = 0;
        std::string runtime_task_id;
    };
    struct QueryResult {
        size_t index = 0;
        uint64_t generation = 0;
        int rc = EIO;
        TaskSnapshot snapshot;
    };

    std::vector<QueryAction> actions;
    {
        std::lock_guard<std::mutex> lock(mu_);
        actions.reserve(nodes_.size());
        for (size_t i = 0; i < nodes_.size(); ++i) {
            auto& node = nodes_[i];
            if (!node.submitted || node.runtime_task_id.empty()) continue;
            if (IsNodeTerminal(node.status) || node.query_inflight) continue;
            node.query_inflight = true;
            QueryAction action;
            action.index = i;
            action.generation = node.generation;
            action.runtime_task_id = node.runtime_task_id;
            actions.push_back(std::move(action));
        }
    }
    if (actions.empty()) return;

    std::vector<QueryResult> results;
    results.reserve(actions.size());
    for (const auto& action : actions) {
        QueryResult result;
        result.index = action.index;
        result.generation = action.generation;
        result.rc = query_fn_(action.runtime_task_id, &result.snapshot);
        results.push_back(std::move(result));
    }

    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& result : results) {
        if (result.index >= nodes_.size()) continue;
        auto& node = nodes_[result.index];
        if (!node.query_inflight) continue;

        node.query_inflight = false;
        if (node.generation != result.generation) {
            continue;
        }
        if (result.rc != 0) continue;

        node.task_snapshot = result.snapshot;
        GroupNodeStatus mapped = MapTaskStatus(node.task_snapshot.status);
        if (node.stop_sent && mapped == GroupNodeStatus::kRunning) {
            mapped = GroupNodeStatus::kStopping;
        }
        node.status = mapped;
        node.error_no = node.task_snapshot.error_code;
        if (node.status == GroupNodeStatus::kFailed && node.error_code.empty()) {
            node.error_code = "STREAM_NODE_RUNTIME_FAILED";
        }
        node.error_message = node.task_snapshot.error_message;
    }
}

bool StreamTaskGroup::HandleFailureOrTimeout(int64_t now_ms) {
    if (status_.load(std::memory_order_acquire) == StreamGroupStatus::kFailed) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& node : nodes_) {
            if (node.status == GroupNodeStatus::kFailed) {
                MarkGroupFailed(
                    node.error_no != 0 ? node.error_no : EIO,
                    "group node failed: " + node.plan.id +
                        (node.error_message.empty() ? "" : (", " + node.error_message)),
                    now_ms,
                    node.error_code.empty() ? "STREAM_GROUP_NODE_FAILED" : node.error_code);
                return true;
            }
        }
    }

    if (timeout_s_ > 0 && started_ms_ > 0) {
        const int64_t elapsed_ms = now_ms - started_ms_;
        if (elapsed_ms >= static_cast<int64_t>(timeout_s_) * 1000) {
            std::vector<std::string> unfinished_nodes;
            {
                std::lock_guard<std::mutex> lock(mu_);
                for (const auto& node : nodes_) {
                    if (!IsNodeTerminal(node.status)) {
                        unfinished_nodes.push_back(node.plan.id);
                    }
                }
            }
            std::string timeout_msg = "stream group timeout: " + std::to_string(timeout_s_) + "s";
            if (!unfinished_nodes.empty()) {
                std::ostringstream oss;
                oss << timeout_msg << ", unfinished_nodes=";
                constexpr size_t kMaxListed = 8;
                const size_t n = std::min(kMaxListed, unfinished_nodes.size());
                for (size_t i = 0; i < n; ++i) {
                    if (i != 0) oss << ",";
                    oss << unfinished_nodes[i];
                }
                if (unfinished_nodes.size() > kMaxListed) {
                    oss << ",...(" << (unfinished_nodes.size() - kMaxListed) << " more)";
                }
                timeout_msg = oss.str();
            }
            MarkGroupFailed(ETIMEDOUT,
                            timeout_msg,
                            now_ms,
                            "STREAM_GROUP_TIMEOUT");
            return true;
        }
    }
    return false;
}

void StreamTaskGroup::HandleStopSignal() {
    struct StopAction {
        size_t index = 0;
        uint64_t generation = 0;
        std::string runtime_task_id;
    };

    bool need_pre_stop = false;
    std::vector<StopAction> actions;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!pre_stop_invoked_ && pre_stop_fn_) {
            pre_stop_invoked_ = true;
            need_pre_stop = true;
        }
        actions.reserve(nodes_.size());
        for (size_t i = 0; i < nodes_.size(); ++i) {
            auto& node = nodes_[i];
            if (!node.submitted || node.runtime_task_id.empty()) {
                if (!IsNodeTerminal(node.status)) {
                    const auto cur_status = status_.load(std::memory_order_acquire);
                    const GroupNodeStatus next_status =
                        (cur_status == StreamGroupStatus::kFailed ||
                         cancel_requested_.load(std::memory_order_acquire))
                            ? GroupNodeStatus::kSkipped
                            : GroupNodeStatus::kStopped;
                    if (node.status != next_status) {
                        node.status = next_status;
                        node.generation += 1;
                    }
                }
                continue;
            }
            if (IsNodeTerminal(node.status)) continue;
            if (node.stop_sent || node.stop_inflight) continue;
            node.stop_sent = true;
            node.stop_inflight = true;
            node.status = GroupNodeStatus::kStopping;
            node.generation += 1;

            StopAction action;
            action.index = i;
            action.generation = node.generation;
            action.runtime_task_id = node.runtime_task_id;
            actions.push_back(std::move(action));
        }
    }

    if (need_pre_stop) {
        pre_stop_fn_();
    }
    for (const auto& action : actions) {
        stop_fn_(action.runtime_task_id);
    }

    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& action : actions) {
        if (action.index >= nodes_.size()) continue;
        auto& node = nodes_[action.index];
        if (!node.stop_inflight) continue;
        node.stop_inflight = false;
        if (node.generation != action.generation) continue;
    }
}

bool StreamTaskGroup::DependenciesSatisfied(const NodeState& node) const {
    if (node.plan.depends_on.empty()) return true;

    for (const auto& dep_id : node.plan.depends_on) {
        auto it = node_index_by_id_.find(dep_id);
        if (it == node_index_by_id_.end()) return false;
        const GroupNodeStatus dep_status = nodes_[it->second].status;
        if (node.plan.start_condition == GroupStartCondition::kOnFinished) {
            if (!IsNodeTerminal(dep_status)) return false;
        } else {
            if (!IsNodeStarted(dep_status)) return false;
        }
    }
    return true;
}

bool StreamTaskGroup::IsNodeTerminal(GroupNodeStatus status) const {
    return status == GroupNodeStatus::kStopped ||
           status == GroupNodeStatus::kCancelled ||
           status == GroupNodeStatus::kFailed ||
           status == GroupNodeStatus::kSkipped;
}

bool StreamTaskGroup::IsNodeStarted(GroupNodeStatus status) const {
    return status == GroupNodeStatus::kRunning ||
           status == GroupNodeStatus::kStopping ||
           status == GroupNodeStatus::kStopped ||
           status == GroupNodeStatus::kCancelled ||
           status == GroupNodeStatus::kFailed;
}

bool StreamTaskGroup::AllNodesTerminal() const {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& node : nodes_) {
        if (!IsNodeTerminal(node.status)) return false;
    }
    return true;
}

void StreamTaskGroup::MarkGroupFailed(int code,
                                      const std::string& message,
                                      int64_t now_ms,
                                      const std::string& error_code) {
    if (status_.load(std::memory_order_acquire) == StreamGroupStatus::kFailed) return;
    status_.store(StreamGroupStatus::kFailed, std::memory_order_release);
    error_code_ = error_code;
    error_no_ = code;
    error_message_ = message;
    stop_requested_.store(true, std::memory_order_release);
    Touch(now_ms);
}

void StreamTaskGroup::TryFinalize(int64_t now_ms) {
    const auto current = status_.load(std::memory_order_acquire);
    if (current == StreamGroupStatus::kFailed) {
        // keep failed
    } else if (cancel_requested_.load(std::memory_order_acquire)) {
        status_.store(StreamGroupStatus::kCancelled, std::memory_order_release);
    } else {
        status_.store(StreamGroupStatus::kStopped, std::memory_order_release);
    }
    finished_ms_ = now_ms;
    Touch(now_ms);
    done_cv_.notify_all();
}

void StreamTaskGroup::Touch(int64_t now_ms) {
    last_active_ms_ = now_ms;
}

}  // namespace scheduler
}  // namespace flowsql
