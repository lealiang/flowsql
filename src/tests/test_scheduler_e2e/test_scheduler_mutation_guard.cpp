#include <cassert>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <string>
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

    std::puts("=== Scheduler mutation guard tests passed ===");
    return 0;
}
