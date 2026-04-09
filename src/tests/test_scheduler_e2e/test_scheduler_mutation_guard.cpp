/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include <cassert>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <framework/interfaces/ichannel.h>
#include <services/scheduler/scheduler_plugin.h>

#define ASSERT_TRUE(expr)                                                                   \
    do {                                                                                    \
        if (!(expr)) {                                                                      \
            std::printf("[FAIL] %s:%d %s\n", __FILE__, __LINE__, #expr);                   \
            std::fflush(stdout);                                                            \
            assert(false);                                                                  \
        }                                                                                   \
    } while (0)

#define ASSERT_EQ(a, b)                                                                     \
    do {                                                                                    \
        auto _a = (a);                                                                      \
        auto _b = (b);                                                                      \
        if (!(_a == _b)) {                                                                  \
            std::printf("[FAIL] %s:%d %s != %s\n", __FILE__, __LINE__, #a, #b);            \
            std::fflush(stdout);                                                            \
            assert(false);                                                                  \
        }                                                                                   \
    } while (0)

namespace flowsql {
namespace scheduler {

class DummyChannel : public IChannel {
 public:
    DummyChannel(std::string category, std::string name, std::string type)
        : category_(std::move(category)), name_(std::move(name)), type_(std::move(type)) {}

    const char* Category() override { return category_.c_str(); }
    const char* Name() override { return name_.c_str(); }
    const char* Type() override { return type_.c_str(); }
    const char* Schema() override { return "{}"; }
    int Open() override { opened_ = true; return 0; }
    int Close() override { opened_ = false; return 0; }
    bool IsOpened() const override { return opened_; }
    int Flush() override { return 0; }

 private:
    std::string category_;
    std::string name_;
    std::string type_;
    bool opened_ = false;
};

struct SchedulerPluginTestAccessor {
    static int TryBeginStreamChannelMutation(SchedulerPlugin* plugin,
                                             const std::string& key,
                                             std::string* reason_out) {
        return plugin->TryBeginStreamChannelMutation(key, reason_out);
    }

    static void EndStreamChannelMutation(SchedulerPlugin* plugin, const std::string& key) {
        plugin->EndStreamChannelMutation(key);
    }

    static int TryAcquireStreamTaskLeases(SchedulerPlugin* plugin,
                                          const std::string& runtime_task_id,
                                          const std::vector<std::string>& source_keys,
                                          const std::vector<std::string>& sink_keys,
                                          std::string* conflict_key_out,
                                          bool* blocked_by_mutation_out,
                                          const std::string& lease_owner_id = "",
                                          const std::unordered_map<std::string, uint64_t>* expected_versions = nullptr,
                                          std::string* version_conflict_key_out = nullptr) {
        return plugin->TryAcquireStreamTaskLeases(runtime_task_id,
                                                  source_keys,
                                                  sink_keys,
                                                  conflict_key_out,
                                                  blocked_by_mutation_out,
                                                  lease_owner_id,
                                                  expected_versions,
                                                  version_conflict_key_out);
    }

    static void ReleaseStreamTaskLeases(SchedulerPlugin* plugin,
                                        const std::string& runtime_task_id) {
        plugin->ReleaseStreamTaskLeases(runtime_task_id);
    }

    static void CaptureStreamChannelVersionSnapshot(
        SchedulerPlugin* plugin,
        const std::vector<std::string>& keys,
        std::unordered_map<std::string, uint64_t>* snapshot_out) {
        plugin->CaptureStreamChannelVersionSnapshot(keys, snapshot_out);
    }

    static void RegisterChannel(SchedulerPlugin* plugin,
                                const std::string& key,
                                std::shared_ptr<IChannel> ch) {
        plugin->RegisterChannel(key, std::move(ch));
    }

    static void EraseManagedChannel(SchedulerPlugin* plugin, const std::string& key) {
        plugin->EraseManagedChannel(key);
    }

    static IChannel* FindChannelWithOwner(SchedulerPlugin* plugin,
                                          const std::string& key,
                                          std::shared_ptr<IChannel>* owner_out) {
        return plugin->FindChannel(key, owner_out);
    }

    static void AddStreamTaskRuntime(SchedulerPlugin* plugin,
                                     const std::string& runtime_task_id,
                                     std::shared_ptr<StreamTask> task) {
        std::lock_guard<std::mutex> lock(plugin->stream_tasks_mu_);
        plugin->stream_tasks_[runtime_task_id] = std::move(task);
    }

    static void AddStreamGroupRuntime(SchedulerPlugin* plugin,
                                      const std::string& runtime_task_id,
                                      std::shared_ptr<StreamTaskGroup> group) {
        std::lock_guard<std::mutex> lock(plugin->stream_task_groups_mu_);
        plugin->stream_task_groups_[runtime_task_id] = std::move(group);
    }

    static void AddGroupNodeOwner(SchedulerPlugin* plugin,
                                  const std::string& node_runtime_task_id,
                                  const std::string& group_runtime_task_id) {
        std::lock_guard<std::mutex> lock(plugin->stream_group_nodes_mu_);
        plugin->stream_group_node_owners_[node_runtime_task_id] = group_runtime_task_id;
    }

    static void AddGroupNodeSources(SchedulerPlugin* plugin,
                                    const std::string& group_runtime_task_id) {
        std::lock_guard<std::mutex> lock(plugin->stream_group_node_sources_mu_);
        GroupNodeResolvedSourceMeta meta;
        meta.sources = {"stream.a"};
        meta.expand_rule = "explicit";
        plugin->stream_group_node_sources_[group_runtime_task_id]["n1"] = std::move(meta);
    }

    static void AddGroupShareSnapshot(SchedulerPlugin* plugin,
                                      const std::string& group_runtime_task_id) {
        std::lock_guard<std::mutex> lock(plugin->stream_group_share_set_snapshots_mu_);
        SharedHubSnapshot snap;
        snap.id = "ss1";
        snap.source_ref = "stream.a";
        plugin->stream_group_share_set_snapshots_[group_runtime_task_id] = {std::move(snap)};
    }

    static bool HasStreamTaskRuntime(SchedulerPlugin* plugin, const std::string& runtime_task_id) {
        std::lock_guard<std::mutex> lock(plugin->stream_tasks_mu_);
        return plugin->stream_tasks_.find(runtime_task_id) != plugin->stream_tasks_.end();
    }

    static bool HasStreamGroupRuntime(SchedulerPlugin* plugin, const std::string& runtime_task_id) {
        std::lock_guard<std::mutex> lock(plugin->stream_task_groups_mu_);
        return plugin->stream_task_groups_.find(runtime_task_id) != plugin->stream_task_groups_.end();
    }

    static bool HasGroupNodeOwner(SchedulerPlugin* plugin, const std::string& node_runtime_task_id) {
        std::lock_guard<std::mutex> lock(plugin->stream_group_nodes_mu_);
        return plugin->stream_group_node_owners_.find(node_runtime_task_id) != plugin->stream_group_node_owners_.end();
    }

    static bool HasGroupNodeSources(SchedulerPlugin* plugin, const std::string& group_runtime_task_id) {
        std::lock_guard<std::mutex> lock(plugin->stream_group_node_sources_mu_);
        return plugin->stream_group_node_sources_.find(group_runtime_task_id) != plugin->stream_group_node_sources_.end();
    }

    static bool HasGroupShareSnapshot(SchedulerPlugin* plugin, const std::string& group_runtime_task_id) {
        std::lock_guard<std::mutex> lock(plugin->stream_group_share_set_snapshots_mu_);
        return plugin->stream_group_share_set_snapshots_.find(group_runtime_task_id) != plugin->stream_group_share_set_snapshots_.end();
    }

    static void MarkRuntimeTerminal(SchedulerPlugin* plugin,
                                    const std::string& runtime_task_id,
                                    const std::string& runtime_kind,
                                    int64_t terminal_ms = 0) {
        plugin->MarkRuntimeTerminal(runtime_task_id, runtime_kind, terminal_ms);
    }

    static void TouchRuntimeAccess(SchedulerPlugin* plugin,
                                   const std::string& runtime_task_id,
                                   int64_t now_ms = 0) {
        plugin->TouchRuntimeAccess(runtime_task_id, now_ms);
    }

    static void SweepRuntimeRetainedObjects(SchedulerPlugin* plugin, int64_t now_ms = 0) {
        plugin->SweepRuntimeRetainedObjects(now_ms);
    }

    static int AcquireStreamExecutionLease(SchedulerPlugin* plugin,
                                           StreamExecutionPlan* plan,
                                           LeaseToken* lease_token,
                                           std::string* err_rsp) {
        return plugin->AcquireStreamExecutionLease(plan, lease_token, err_rsp);
    }

    static bool HasTaskLease(SchedulerPlugin* plugin, const std::string& runtime_task_id) {
        std::lock_guard<std::mutex> lock(plugin->stream_channel_refs_mu_);
        return plugin->stream_task_leases_.find(runtime_task_id) != plugin->stream_task_leases_.end();
    }

    static uint32_t ChannelRefCount(SchedulerPlugin* plugin, const std::string& channel_key) {
        std::lock_guard<std::mutex> lock(plugin->stream_channel_refs_mu_);
        auto it = plugin->stream_channel_ref_counts_.find(channel_key);
        return it == plugin->stream_channel_ref_counts_.end() ? 0u : it->second;
    }
};
}  // namespace scheduler
}  // namespace flowsql

int main() {
    std::puts("=== Scheduler mutation guard tests ===");

    flowsql::scheduler::SchedulerPlugin plugin;
    const std::string source_key = "ring.in";
    const std::string sink_key = "ring.out";
    const std::vector<std::string> source_keys = {source_key};
    const std::vector<std::string> sink_keys = {sink_key};

    std::string reason;
    ASSERT_EQ(flowsql::scheduler::SchedulerPluginTestAccessor::TryBeginStreamChannelMutation(&plugin, source_key, &reason), 0);
    ASSERT_TRUE(reason.empty());

    std::string conflict_key;
    bool blocked_by_mutation = false;
    ASSERT_EQ(flowsql::scheduler::SchedulerPluginTestAccessor::TryAcquireStreamTaskLeases(
                  &plugin, "stream_task_1", source_keys, sink_keys, &conflict_key, &blocked_by_mutation),
              EBUSY);
    ASSERT_TRUE(blocked_by_mutation);
    ASSERT_EQ(conflict_key, source_key);

    flowsql::scheduler::SchedulerPluginTestAccessor::EndStreamChannelMutation(&plugin, source_key);

    conflict_key.clear();
    blocked_by_mutation = false;
    ASSERT_EQ(flowsql::scheduler::SchedulerPluginTestAccessor::TryAcquireStreamTaskLeases(
                  &plugin, "stream_task_2", source_keys, sink_keys, &conflict_key, &blocked_by_mutation),
              0);
    ASSERT_TRUE(!blocked_by_mutation);

    reason.clear();
    ASSERT_EQ(flowsql::scheduler::SchedulerPluginTestAccessor::TryBeginStreamChannelMutation(&plugin, source_key, &reason), EBUSY);
    ASSERT_EQ(reason, "in_use");

    flowsql::scheduler::SchedulerPluginTestAccessor::ReleaseStreamTaskLeases(&plugin, "stream_task_2");
    flowsql::scheduler::SchedulerPluginTestAccessor::EndStreamChannelMutation(&plugin, source_key);

    // Group lease owner reuse: same owner can share source lease across nodes.
    conflict_key.clear();
    blocked_by_mutation = false;
    ASSERT_EQ(flowsql::scheduler::SchedulerPluginTestAccessor::TryAcquireStreamTaskLeases(
                  &plugin,
                  "stream_group_node_1",
                  source_keys,
                  sink_keys,
                  &conflict_key,
                  &blocked_by_mutation,
                  "stream_group_1"),
              0);
    ASSERT_EQ(flowsql::scheduler::SchedulerPluginTestAccessor::TryAcquireStreamTaskLeases(
                  &plugin,
                  "stream_group_node_2",
                  source_keys,
                  sink_keys,
                  &conflict_key,
                  &blocked_by_mutation,
                  "stream_group_1"),
              0);
    conflict_key.clear();
    ASSERT_EQ(flowsql::scheduler::SchedulerPluginTestAccessor::TryAcquireStreamTaskLeases(
                  &plugin,
                  "stream_group_other",
                  source_keys,
                  sink_keys,
                  &conflict_key,
                  &blocked_by_mutation,
                  "stream_group_2"),
              EBUSY);
    ASSERT_EQ(conflict_key, source_key);
    flowsql::scheduler::SchedulerPluginTestAccessor::ReleaseStreamTaskLeases(&plugin, "stream_group_node_1");
    flowsql::scheduler::SchedulerPluginTestAccessor::ReleaseStreamTaskLeases(&plugin, "stream_group_node_2");

    // TOCTOU version guard: stale snapshot should be rejected with EAGAIN.
    const std::vector<std::string> lease_keys = {source_key, sink_key};
    std::unordered_map<std::string, uint64_t> snapshot_before;
    flowsql::scheduler::SchedulerPluginTestAccessor::CaptureStreamChannelVersionSnapshot(
        &plugin, lease_keys, &snapshot_before);
    const uint64_t baseline_version = snapshot_before[source_key];

    reason.clear();
    ASSERT_EQ(flowsql::scheduler::SchedulerPluginTestAccessor::TryBeginStreamChannelMutation(
                  &plugin, source_key, &reason),
              0);
    flowsql::scheduler::SchedulerPluginTestAccessor::EndStreamChannelMutation(&plugin, source_key);

    std::string version_conflict_key;
    conflict_key.clear();
    blocked_by_mutation = false;
    ASSERT_EQ(flowsql::scheduler::SchedulerPluginTestAccessor::TryAcquireStreamTaskLeases(
                  &plugin,
                  "stream_task_version_old",
                  source_keys,
                  sink_keys,
                  &conflict_key,
                  &blocked_by_mutation,
                  "",
                  &snapshot_before,
                  &version_conflict_key),
              EAGAIN);
    ASSERT_EQ(conflict_key, source_key);
    ASSERT_EQ(version_conflict_key, source_key);

    std::unordered_map<std::string, uint64_t> snapshot_after;
    flowsql::scheduler::SchedulerPluginTestAccessor::CaptureStreamChannelVersionSnapshot(
        &plugin, lease_keys, &snapshot_after);
    ASSERT_EQ(snapshot_after[source_key], baseline_version + 1u);
    conflict_key.clear();
    version_conflict_key.clear();
    blocked_by_mutation = false;
    ASSERT_EQ(flowsql::scheduler::SchedulerPluginTestAccessor::TryAcquireStreamTaskLeases(
                  &plugin,
                  "stream_task_version_new",
                  source_keys,
                  sink_keys,
                  &conflict_key,
                  &blocked_by_mutation,
                  "",
                  &snapshot_after,
                  &version_conflict_key),
              0);
    flowsql::scheduler::SchedulerPluginTestAccessor::ReleaseStreamTaskLeases(&plugin, "stream_task_version_new");

    // AcquireStreamExecutionLease: when execution aborts before commit, lease must be auto-released.
    {
        flowsql::scheduler::StreamExecutionPlan plan;
        plan.runtime_task_id = "stream_task_raii_no_commit";
        plan.source_keys = source_keys;
        plan.sink_keys = sink_keys;
        plan.skip_lease_acquire = false;

        std::string lease_err;
        {
            flowsql::scheduler::LeaseToken lease_token;
            ASSERT_EQ(flowsql::scheduler::SchedulerPluginTestAccessor::AcquireStreamExecutionLease(
                          &plugin, &plan, &lease_token, &lease_err),
                      0);
            ASSERT_TRUE(flowsql::scheduler::SchedulerPluginTestAccessor::HasTaskLease(
                &plugin, "stream_task_raii_no_commit"));
            ASSERT_EQ(flowsql::scheduler::SchedulerPluginTestAccessor::ChannelRefCount(&plugin, source_key), 1u);
            ASSERT_EQ(flowsql::scheduler::SchedulerPluginTestAccessor::ChannelRefCount(&plugin, sink_key), 1u);
        }
        ASSERT_TRUE(!flowsql::scheduler::SchedulerPluginTestAccessor::HasTaskLease(
            &plugin, "stream_task_raii_no_commit"));
        ASSERT_EQ(flowsql::scheduler::SchedulerPluginTestAccessor::ChannelRefCount(&plugin, source_key), 0u);
        ASSERT_EQ(flowsql::scheduler::SchedulerPluginTestAccessor::ChannelRefCount(&plugin, sink_key), 0u);
    }

    // AcquireStreamExecutionLease: commit should transfer ownership to runtime and skip auto-release.
    {
        flowsql::scheduler::StreamExecutionPlan plan;
        plan.runtime_task_id = "stream_task_raii_commit";
        plan.source_keys = source_keys;
        plan.sink_keys = sink_keys;
        plan.skip_lease_acquire = false;

        std::string lease_err;
        {
            flowsql::scheduler::LeaseToken lease_token;
            ASSERT_EQ(flowsql::scheduler::SchedulerPluginTestAccessor::AcquireStreamExecutionLease(
                          &plugin, &plan, &lease_token, &lease_err),
                      0);
            lease_token.Commit();
        }
        ASSERT_TRUE(flowsql::scheduler::SchedulerPluginTestAccessor::HasTaskLease(
            &plugin, "stream_task_raii_commit"));
        flowsql::scheduler::SchedulerPluginTestAccessor::ReleaseStreamTaskLeases(
            &plugin, "stream_task_raii_commit");
        ASSERT_TRUE(!flowsql::scheduler::SchedulerPluginTestAccessor::HasTaskLease(
            &plugin, "stream_task_raii_commit"));
    }

    // Managed channel owner should remain alive after map erase if caller holds shared owner.
    {
        const std::string managed_key = "stream.test_managed";
        auto managed = std::make_shared<flowsql::scheduler::DummyChannel>(
            "stream", "test_managed", flowsql::ChannelType::kStream);
        flowsql::scheduler::SchedulerPluginTestAccessor::RegisterChannel(&plugin, managed_key, managed);

        std::shared_ptr<flowsql::IChannel> owner;
        flowsql::IChannel* raw = flowsql::scheduler::SchedulerPluginTestAccessor::FindChannelWithOwner(
            &plugin, managed_key, &owner);
        ASSERT_TRUE(raw != nullptr);
        ASSERT_TRUE(owner != nullptr);
        ASSERT_EQ(raw, owner.get());

        flowsql::scheduler::SchedulerPluginTestAccessor::EraseManagedChannel(&plugin, managed_key);
        ASSERT_EQ(std::string(owner->Type()), std::string(flowsql::ChannelType::kStream));
    }

    // StreamTaskGroup stop semantics: stopped group must not mark non-submitted nodes as skipped.
    {
        std::vector<flowsql::scheduler::GroupNodePlan> nodes;
        flowsql::scheduler::GroupNodePlan n1;
        n1.id = "n1";
        n1.sql = "SELECT 1";
        nodes.push_back(n1);

        std::atomic<int> submit_calls{0};
        flowsql::scheduler::StreamTaskGroup group(
            "g_stop",
            "g_stop",
            nodes,
            0,
            [&submit_calls](const std::string&, const std::string&, std::string* runtime_task_id, std::string*) {
                submit_calls.fetch_add(1);
                if (runtime_task_id) *runtime_task_id = "node_rt";
                return 0;
            },
            [](const std::string&, flowsql::scheduler::TaskSnapshot* out) {
                if (!out) return EINVAL;
                out->status = flowsql::scheduler::StreamTaskStatus::kRunning;
                return 0;
            },
            [](const std::string&) {});

        group.RequestStop(false);
        std::string start_err;
        ASSERT_EQ(group.Start(&start_err), 0);
        group.Join();
        auto snapshot = group.Snapshot();
        ASSERT_EQ(snapshot.status, flowsql::scheduler::StreamGroupStatus::kStopped);
        ASSERT_EQ(snapshot.nodes.size(), 1u);
        ASSERT_EQ(snapshot.nodes[0].status, flowsql::scheduler::GroupNodeStatus::kStopped);
        ASSERT_EQ(submit_calls.load(), 0);
    }

    // StreamTaskGroup cancel semantics: cancelled group can mark non-submitted nodes as skipped.
    {
        std::vector<flowsql::scheduler::GroupNodePlan> nodes;
        flowsql::scheduler::GroupNodePlan n1;
        n1.id = "n1";
        n1.sql = "SELECT 1";
        nodes.push_back(n1);

        flowsql::scheduler::StreamTaskGroup group(
            "g_cancel",
            "g_cancel",
            nodes,
            0,
            [](const std::string&, const std::string&, std::string* runtime_task_id, std::string*) {
                if (runtime_task_id) *runtime_task_id = "node_rt";
                return 0;
            },
            [](const std::string&, flowsql::scheduler::TaskSnapshot* out) {
                if (!out) return EINVAL;
                out->status = flowsql::scheduler::StreamTaskStatus::kRunning;
                return 0;
            },
            [](const std::string&) {});

        group.RequestStop(true);
        std::string start_err;
        ASSERT_EQ(group.Start(&start_err), 0);
        group.Join();
        auto snapshot = group.Snapshot();
        ASSERT_EQ(snapshot.status, flowsql::scheduler::StreamGroupStatus::kCancelled);
        ASSERT_EQ(snapshot.nodes.size(), 1u);
        ASSERT_EQ(snapshot.nodes[0].status, flowsql::scheduler::GroupNodeStatus::kSkipped);
    }

    // StreamTaskGroup timeout message should contain unfinished node ids.
    {
        std::vector<flowsql::scheduler::GroupNodePlan> nodes;
        flowsql::scheduler::GroupNodePlan n1;
        n1.id = "n1";
        n1.sql = "SELECT 1";
        nodes.push_back(n1);

        std::atomic<bool> stop_called{false};
        flowsql::scheduler::StreamTaskGroup group(
            "g_timeout",
            "g_timeout",
            nodes,
            1,
            [](const std::string&, const std::string&, std::string* runtime_task_id, std::string*) {
                if (runtime_task_id) *runtime_task_id = "node_rt";
                return 0;
            },
            [&stop_called](const std::string&, flowsql::scheduler::TaskSnapshot* out) {
                if (!out) return EINVAL;
                out->status = stop_called.load()
                                  ? flowsql::scheduler::StreamTaskStatus::kStopped
                                  : flowsql::scheduler::StreamTaskStatus::kRunning;
                return 0;
            },
            [&stop_called](const std::string&) { stop_called.store(true); });

        std::string start_err;
        ASSERT_EQ(group.Start(&start_err), 0);
        group.Join();
        auto snapshot = group.Snapshot();
        ASSERT_EQ(snapshot.status, flowsql::scheduler::StreamGroupStatus::kFailed);
        ASSERT_EQ(snapshot.error_code, "STREAM_GROUP_TIMEOUT");
        ASSERT_TRUE(snapshot.error_message.find("unfinished_nodes=n1") != std::string::npos);
    }

    // StreamTaskGroup slow submit/query callback must not block Snapshot lock path.
    {
        std::vector<flowsql::scheduler::GroupNodePlan> nodes;
        flowsql::scheduler::GroupNodePlan n1;
        n1.id = "n1";
        n1.sql = "SELECT 1";
        nodes.push_back(n1);

        std::atomic<bool> submit_entered{false};
        flowsql::scheduler::StreamTaskGroup group(
            "g_snapshot_latency",
            "g_snapshot_latency",
            nodes,
            0,
            [&submit_entered](const std::string&, const std::string&, std::string* runtime_task_id, std::string*) {
                submit_entered.store(true, std::memory_order_release);
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                if (runtime_task_id) *runtime_task_id = "node_rt";
                return 0;
            },
            [](const std::string&, flowsql::scheduler::TaskSnapshot* out) {
                if (!out) return EINVAL;
                out->status = flowsql::scheduler::StreamTaskStatus::kStopped;
                return 0;
            },
            [](const std::string&) {});

        std::string start_err;
        ASSERT_EQ(group.Start(&start_err), 0);

        bool observed_submit = false;
        for (int i = 0; i < 200; ++i) {
            if (submit_entered.load(std::memory_order_acquire)) {
                observed_submit = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_TRUE(observed_submit);

        const auto begin = std::chrono::steady_clock::now();
        const auto snapshot_mid = group.Snapshot();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin).count();
        ASSERT_EQ(snapshot_mid.nodes.size(), 1u);
        ASSERT_TRUE(elapsed_ms < 120);

        group.Join();
        const auto final_snapshot = group.Snapshot();
        ASSERT_EQ(final_snapshot.status, flowsql::scheduler::StreamGroupStatus::kStopped);
    }

    // Scheduler runtime retention: keep newest terminal runtime by max_count.
    {
        flowsql::scheduler::SchedulerPlugin retention_plugin;
        ASSERT_EQ(retention_plugin.Option("stream_runtime_retention_s=3600;stream_runtime_max_count=1"), 0);

        flowsql::scheduler::StreamRuntime runtime;
        auto old_task = std::make_shared<flowsql::scheduler::StreamTask>("runtime_old", &runtime);
        old_task->SetFailedOnce(EIO, "old failed");
        auto new_task = std::make_shared<flowsql::scheduler::StreamTask>("runtime_new", &runtime);
        new_task->SetFailedOnce(EIO, "new failed");

        flowsql::scheduler::SchedulerPluginTestAccessor::AddStreamTaskRuntime(
            &retention_plugin, "runtime_old", old_task);
        flowsql::scheduler::SchedulerPluginTestAccessor::AddStreamTaskRuntime(
            &retention_plugin, "runtime_new", new_task);
        flowsql::scheduler::SchedulerPluginTestAccessor::MarkRuntimeTerminal(
            &retention_plugin, "runtime_old", "single");
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        flowsql::scheduler::SchedulerPluginTestAccessor::MarkRuntimeTerminal(
            &retention_plugin, "runtime_new", "single");
        flowsql::scheduler::SchedulerPluginTestAccessor::TouchRuntimeAccess(
            &retention_plugin, "runtime_old");
        flowsql::scheduler::SchedulerPluginTestAccessor::TouchRuntimeAccess(
            &retention_plugin, "runtime_new");

        flowsql::scheduler::SchedulerPluginTestAccessor::SweepRuntimeRetainedObjects(&retention_plugin);
        ASSERT_TRUE(!flowsql::scheduler::SchedulerPluginTestAccessor::HasStreamTaskRuntime(
            &retention_plugin, "runtime_old"));
        ASSERT_TRUE(flowsql::scheduler::SchedulerPluginTestAccessor::HasStreamTaskRuntime(
            &retention_plugin, "runtime_new"));
    }

    // Scheduler runtime retention: group GC should cleanup related runtime indexes.
    {
        flowsql::scheduler::SchedulerPlugin retention_plugin;
        ASSERT_EQ(retention_plugin.Option("stream_runtime_retention_s=0;stream_runtime_max_count=100"), 0);

        std::vector<flowsql::scheduler::GroupNodePlan> nodes;
        flowsql::scheduler::GroupNodePlan n1;
        n1.id = "n1";
        n1.sql = "SELECT 1";
        nodes.push_back(n1);
        auto group = std::make_shared<flowsql::scheduler::StreamTaskGroup>(
            "runtime_group_old",
            "runtime_group_old",
            nodes,
            0,
            [](const std::string&, const std::string&, std::string* runtime_task_id, std::string*) {
                if (runtime_task_id) *runtime_task_id = "node_rt";
                return 0;
            },
            [](const std::string&, flowsql::scheduler::TaskSnapshot* out) {
                if (!out) return EINVAL;
                out->status = flowsql::scheduler::StreamTaskStatus::kStopped;
                return 0;
            },
            [](const std::string&) {});
        group->MarkExternalFailed(EIO, "group failed", "STREAM_GROUP_EXTERNAL_FAILED");

        flowsql::scheduler::SchedulerPluginTestAccessor::AddStreamGroupRuntime(
            &retention_plugin, "runtime_group_old", group);
        flowsql::scheduler::SchedulerPluginTestAccessor::AddGroupNodeOwner(
            &retention_plugin, "runtime_group_node_old", "runtime_group_old");
        flowsql::scheduler::SchedulerPluginTestAccessor::AddGroupNodeSources(
            &retention_plugin, "runtime_group_old");
        flowsql::scheduler::SchedulerPluginTestAccessor::AddGroupShareSnapshot(
            &retention_plugin, "runtime_group_old");
        flowsql::scheduler::SchedulerPluginTestAccessor::MarkRuntimeTerminal(
            &retention_plugin, "runtime_group_old", "group", 1);

        flowsql::scheduler::SchedulerPluginTestAccessor::SweepRuntimeRetainedObjects(
            &retention_plugin, 1000);
        ASSERT_TRUE(!flowsql::scheduler::SchedulerPluginTestAccessor::HasStreamGroupRuntime(
            &retention_plugin, "runtime_group_old"));
        ASSERT_TRUE(!flowsql::scheduler::SchedulerPluginTestAccessor::HasGroupNodeOwner(
            &retention_plugin, "runtime_group_node_old"));
        ASSERT_TRUE(!flowsql::scheduler::SchedulerPluginTestAccessor::HasGroupNodeSources(
            &retention_plugin, "runtime_group_old"));
        ASSERT_TRUE(!flowsql::scheduler::SchedulerPluginTestAccessor::HasGroupShareSnapshot(
            &retention_plugin, "runtime_group_old"));
    }

    std::puts("=== Scheduler mutation guard tests passed ===");
    return 0;
}
