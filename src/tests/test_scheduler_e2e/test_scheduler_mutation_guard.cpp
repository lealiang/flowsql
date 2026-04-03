#include <cassert>
#include <cerrno>
#include <cstdio>
#include <string>
#include <vector>

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
                                          bool* blocked_by_mutation_out) {
        return plugin->TryAcquireStreamTaskLeases(runtime_task_id,
                                                  source_keys,
                                                  sink_keys,
                                                  conflict_key_out,
                                                  blocked_by_mutation_out);
    }

    static void ReleaseStreamTaskLeases(SchedulerPlugin* plugin,
                                        const std::string& runtime_task_id) {
        plugin->ReleaseStreamTaskLeases(runtime_task_id);
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

    std::puts("=== Scheduler mutation guard tests passed ===");
    return 0;
}
