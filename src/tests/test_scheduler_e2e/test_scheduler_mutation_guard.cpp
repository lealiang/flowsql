// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <arrow/api.h>

#include <cassert>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <framework/core/packet_codec.h>
#include <framework/core/ring_stream_channel.h>
#include <framework/core/sql_parser.h>
#include <framework/interfaces/iblock_stream_factory.h>
#include <framework/interfaces/iblock_stream_manager.h>
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

bool SameGuid(const Guid& left, const Guid& right) {
    return !(left < right) && !(right < left);
}

class TraversalBlockFactory final : public IBlockStreamFactory {
 public:
    explicit TraversalBlockFactory(int id) : id(id) {}

    IBlockStreamChannel* Get(const char* requested_type, const char* requested_name) override {
        ++get_calls;
        if (!channel || !requested_type || !requested_name) return nullptr;
        return type == requested_type && name == requested_name ? channel : nullptr;
    }
    void List(std::function<void(const char*, const char*, IBlockStreamChannel*)> callback) override {
        ++list_calls;
        if (channel && callback) callback(type.c_str(), name.c_str(), channel);
    }

    int id;
    std::string type = "pcapfile";
    std::string name = "input";
    IBlockStreamChannel* channel = nullptr;
    int get_calls = 0;
    int list_calls = 0;
};

class TraversalBlockManager final : public IBlockStreamManager {
 public:
    explicit TraversalBlockManager(int id) : id(id) {}

    int AddChannel(const std::string& requested_type,
                   const std::string& requested_name,
                   const std::string& requested_option) override {
        ++add_calls;
        last_add_type = requested_type;
        last_add_name = requested_name;
        last_add_option = requested_option;
        return add_rc;
    }
    int ModifyChannel(const std::string& requested_type,
                      const std::string& requested_name,
                      const std::string& requested_option) override {
        ++modify_calls;
        last_modify_type = requested_type;
        last_modify_name = requested_name;
        last_modify_option = requested_option;
        return modify_rc;
    }
    int RemoveChannel(const std::string& requested_type, const std::string& requested_name) override {
        ++remove_calls;
        last_remove_type = requested_type;
        last_remove_name = requested_name;
        return remove_rc;
    }
    void QueryChannels(
        std::function<void(const std::string&, const std::string&, const std::string&, const std::string&)> callback)
        override {
        ++query_calls;
        if (owns_channel && callback) callback(type, name, "{}", "opened");
    }

    int id;
    std::string type = "pcapfile";
    std::string name = "input";
    bool owns_channel = false;
    int add_rc = ENOTSUP;
    int modify_rc = ENOENT;
    int remove_rc = ENOENT;
    int add_calls = 0;
    int modify_calls = 0;
    int remove_calls = 0;
    int query_calls = 0;
    std::string last_add_type;
    std::string last_add_name;
    std::string last_add_option;
    std::string last_modify_type;
    std::string last_modify_name;
    std::string last_modify_option;
    std::string last_remove_type;
    std::string last_remove_name;
};

class RoutingBlockChannel final : public IBlockStreamChannel {
 public:
    explicit RoutingBlockChannel(std::string name) : name_(std::move(name)) {}

    const char* Category() override { return "pcapfile"; }
    const char* Name() override { return name_.c_str(); }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return "packet"; }
    int Open() override { return 0; }
    int Close() override { return 0; }
    bool IsOpened() const override { return true; }
    int Flush() override { return 0; }
    BlockPollEvent PollBlock(int) override { return {BlockPollEvent::kEof, nullptr, 0}; }
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>&) override { return EINVAL; }
    void Cancel() override {}
    bool IsFinished() const override { return true; }

 private:
    std::string name_;
};

class SchemaBlockChannel final : public IBlockStreamChannel {
 public:
    explicit SchemaBlockChannel(std::shared_ptr<arrow::RecordBatch> batch,
                                int data_count = 1)
        : batch_(std::move(batch)), data_count_(data_count) {}

    const char* Category() override { return "pcapfile"; }
    const char* Name() override { return "input"; }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return "packet"; }
    int Open() override { return 0; }
    int Close() override { return 0; }
    bool IsOpened() const override { return true; }
    int Flush() override { return 0; }
    BlockPollEvent PollBlock(int) override {
        ++poll_calls;
        if (emitted_timeout_count_ < timeout_count) {
            ++emitted_timeout_count_;
            return {BlockPollEvent::kTimeout, nullptr, 0};
        }
        if (emitted_count_ >= data_count_) return {terminal_kind, nullptr, terminal_err};
        ++emitted_count_;
        return {BlockPollEvent::kData, batch_, 0};
    }
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& batch) override {
        if (batch != batch_) return EINVAL;
        ++release_calls;
        return release_rc;
    }
    void Cancel() override {}
    bool IsFinished() const override { return emitted_count_ >= data_count_; }

    int release_calls = 0;
    int release_rc = 0;
    int poll_calls = 0;
    int timeout_count = 0;
    BlockPollEvent::Kind terminal_kind = BlockPollEvent::kEof;
    int terminal_err = 0;

 private:
    std::shared_ptr<arrow::RecordBatch> batch_;
    int data_count_ = 1;
    int emitted_count_ = 0;
    int emitted_timeout_count_ = 0;
};

class SchemaBlockOperator final : public IBlockStreamOperator {
 public:
    std::string Category() override { return "test"; }
    std::string Name() override { return "schema_guard"; }
    std::string Description() override { return "block schema guard test operator"; }
    int Configure(const char*, const char*) override { return 0; }
    int Init(const char*) override {
        ++init_calls;
        return 0;
    }
    int OnSchemaReady(std::shared_ptr<arrow::Schema> schema) override {
        ++schema_calls;
        schema_matches_packet = schema && schema->Equals(packet::PacketSchema());
        return schema_rc;
    }
    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>&, int64_t) override {
        schema_before_process = schema_calls == 1;
        ++process_calls;
        if (throw_on_process) throw std::runtime_error("process block failed");
        if (process_rc != 0) return process_rc;
        return process_calls >= process_stop_after ? 1 : 0;
    }
    int Flush() override {
        ++flush_calls;
        return 0;
    }
    std::string LastError() override { return "schema rejected"; }

    int schema_rc = 0;
    int process_rc = 0;
    int process_stop_after = 1;
    int init_calls = 0;
    int schema_calls = 0;
    int process_calls = 0;
    int flush_calls = 0;
    bool throw_on_process = false;
    bool schema_before_process = false;
    bool schema_matches_packet = false;
};

class BlockProviderQuerier final : public IQuerier {
 public:
    int Traverse(const Guid& iid, fntraverse callback) override {
        const std::vector<void*>* providers = nullptr;
        if (SameGuid(iid, IID_BLOCK_STREAM_FACTORY)) {
            ++factory_traverse_calls;
            providers = &factories;
        } else if (SameGuid(iid, IID_BLOCK_STREAM_MANAGER)) {
            ++manager_traverse_calls;
            providers = &managers;
        } else if (SameGuid(iid, IID_BLOCK_STREAM_OPERATOR)) {
            ++operator_traverse_calls;
            providers = &operators;
        } else {
            ++unexpected_traverse_calls;
            return 0;
        }
        if (!callback) return 0;
        for (void* provider : *providers) {
            const int rc = callback(provider);
            if (rc == -1) return rc;
        }
        return 0;
    }

    void* First(const Guid& iid) override {
        ++first_calls;
        if (SameGuid(iid, IID_BLOCK_STREAM_FACTORY)) ++block_factory_first_calls;
        if (SameGuid(iid, IID_BLOCK_STREAM_MANAGER)) ++block_manager_first_calls;
        return nullptr;
    }

    std::vector<void*> factories;
    std::vector<void*> managers;
    std::vector<void*> operators;
    int factory_traverse_calls = 0;
    int manager_traverse_calls = 0;
    int operator_traverse_calls = 0;
    int unexpected_traverse_calls = 0;
    int first_calls = 0;
    int block_factory_first_calls = 0;
    int block_manager_first_calls = 0;
};

struct SchedulerPluginTestAccessor {
    static size_t TraverseBlockFactories(
        SchedulerPlugin* plugin,
        const std::function<int(IBlockStreamFactory*)>& visitor) {
        return plugin->TraverseBlockFactories(visitor);
    }

    static size_t TraverseBlockManagers(
        SchedulerPlugin* plugin,
        const std::function<int(IBlockStreamManager*)>& visitor) {
        return plugin->TraverseBlockManagers(visitor);
    }

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

    static IChannel* FindChannelWithAmbiguity(SchedulerPlugin* plugin,
                                              const std::string& key,
                                              std::shared_ptr<IChannel>* owner_out,
                                              bool* ambiguous_out) {
        return plugin->FindChannel(key, owner_out, ambiguous_out);
    }

    static int32_t ResolveSourceBindings(SchedulerPlugin* plugin,
                                         const SqlStatement& stmt,
                                         std::string* response) {
        SchedulerPlugin::SourceResolveResult result;
        return plugin->ResolveSourceBindings(stmt, &result, response);
    }

    static int32_t HandleAddStreamChannel(SchedulerPlugin* plugin,
                                          const std::string& request,
                                          std::string* response) {
        return plugin->HandleAddStreamChannel("", request, *response);
    }

    static int32_t HandleModifyStreamChannel(SchedulerPlugin* plugin,
                                             const std::string& request,
                                             std::string* response) {
        return plugin->HandleModifyStreamChannel("", request, *response);
    }

    static int32_t HandleRemoveStreamChannel(SchedulerPlugin* plugin,
                                             const std::string& request,
                                             std::string* response) {
        return plugin->HandleRemoveStreamChannel("", request, *response);
    }

    static int32_t HandleExecute(SchedulerPlugin* plugin,
                                 const std::string& request,
                                 std::string* response) {
        return plugin->HandleExecute("", request, *response);
    }

    static int ExecuteBlockOperator(SchedulerPlugin* plugin,
                                    IBlockStreamChannel* source,
                                    IBlockStreamOperator* op,
                                    int64_t* rows_affected,
                                    std::string* error) {
        SchedulerPlugin::BlockExecutionTerminal terminal;
        return plugin->ExecuteBlockOperator(source, op, &terminal, rows_affected, error);
    }

    static int StartBatchRuntime(SchedulerPlugin* plugin) {
        return plugin->batch_runtime_.Start(
            1,
            [plugin](const std::string& sql, std::string* response) -> int32_t {
                if (!response) return error::INTERNAL_ERROR;
                const std::string request = "{\"sql\":\"" + sql + "\"}";
                return plugin->HandleExecute("", request, *response);
            });
    }

    static void StopBatchRuntime(SchedulerPlugin* plugin) {
        plugin->batch_runtime_.Stop();
    }

    static int32_t HandleBatchSubmit(SchedulerPlugin* plugin,
                                     const std::string& request,
                                     std::string* response) {
        return plugin->HandleBatchSubmit("", request, *response);
    }

    static int QueryBatchSnapshot(SchedulerPlugin* plugin,
                                  const std::string& runtime_task_id,
                                  BatchRuntimeSnapshot* snapshot) {
        return plugin->batch_runtime_.Query(runtime_task_id, snapshot);
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

void TestBlockProviderTraversal() {
    {
        BlockProviderQuerier querier;
        SchedulerPlugin plugin;
        ASSERT_EQ(plugin.Load(&querier), 0);
        int factory_visits = 0;
        int manager_visits = 0;
        ASSERT_EQ(SchedulerPluginTestAccessor::TraverseBlockFactories(
                      &plugin, [&](IBlockStreamFactory*) {
                          ++factory_visits;
                          return 0;
                      }),
                  0u);
        ASSERT_EQ(SchedulerPluginTestAccessor::TraverseBlockManagers(
                      &plugin, [&](IBlockStreamManager*) {
                          ++manager_visits;
                          return 0;
                      }),
                  0u);
        ASSERT_EQ(factory_visits, 0);
        ASSERT_EQ(manager_visits, 0);
        ASSERT_EQ(querier.factory_traverse_calls, 1);
        ASSERT_EQ(querier.manager_traverse_calls, 1);
        ASSERT_EQ(querier.unexpected_traverse_calls, 0);
        ASSERT_EQ(querier.first_calls, 0);
    }

    TraversalBlockFactory factory_one(1);
    TraversalBlockFactory factory_two(2);
    TraversalBlockManager manager_one(1);
    TraversalBlockManager manager_two(2);
    {
        BlockProviderQuerier querier;
        querier.factories = {&factory_one};
        querier.managers = {&manager_one};
        SchedulerPlugin plugin;
        ASSERT_EQ(plugin.Load(&querier), 0);
        std::vector<int> factory_ids;
        std::vector<int> manager_ids;
        ASSERT_EQ(SchedulerPluginTestAccessor::TraverseBlockFactories(
                      &plugin, [&](IBlockStreamFactory* provider) {
                          factory_ids.push_back(static_cast<TraversalBlockFactory*>(provider)->id);
                          return 0;
                      }),
                  1u);
        ASSERT_EQ(SchedulerPluginTestAccessor::TraverseBlockManagers(
                      &plugin, [&](IBlockStreamManager* provider) {
                          manager_ids.push_back(static_cast<TraversalBlockManager*>(provider)->id);
                          return 0;
                      }),
                  1u);
        ASSERT_EQ(factory_ids, std::vector<int>({1}));
        ASSERT_EQ(manager_ids, std::vector<int>({1}));
        ASSERT_EQ(querier.factory_traverse_calls, 1);
        ASSERT_EQ(querier.manager_traverse_calls, 1);
        ASSERT_EQ(querier.unexpected_traverse_calls, 0);
        ASSERT_EQ(querier.first_calls, 0);
    }
    {
        BlockProviderQuerier querier;
        querier.factories = {&factory_one, nullptr, &factory_two};
        querier.managers = {&manager_one, nullptr, &manager_two};
        SchedulerPlugin plugin;
        ASSERT_EQ(plugin.Load(&querier), 0);
        std::vector<int> factory_ids;
        std::vector<int> manager_ids;
        ASSERT_EQ(SchedulerPluginTestAccessor::TraverseBlockFactories(
                      &plugin, [&](IBlockStreamFactory* provider) {
                          factory_ids.push_back(static_cast<TraversalBlockFactory*>(provider)->id);
                          return 0;
                      }),
                  2u);
        ASSERT_EQ(SchedulerPluginTestAccessor::TraverseBlockManagers(
                      &plugin, [&](IBlockStreamManager* provider) {
                          manager_ids.push_back(static_cast<TraversalBlockManager*>(provider)->id);
                          return 0;
                      }),
                  2u);
        ASSERT_EQ(factory_ids, std::vector<int>({1, 2}));
        ASSERT_EQ(manager_ids, std::vector<int>({1, 2}));
        ASSERT_EQ(SchedulerPluginTestAccessor::TraverseBlockFactories(
                      &plugin, [](IBlockStreamFactory*) { return -1; }),
                  1u);
        ASSERT_EQ(SchedulerPluginTestAccessor::TraverseBlockManagers(
                      &plugin, [](IBlockStreamManager*) { return -1; }),
                  1u);
        ASSERT_EQ(querier.factory_traverse_calls, 2);
        ASSERT_EQ(querier.manager_traverse_calls, 2);
        ASSERT_EQ(querier.unexpected_traverse_calls, 0);
        ASSERT_EQ(querier.first_calls, 0);
    }
}

void TestBlockSourceAndManagerRouting() {
    RoutingBlockChannel channel_one("input");
    RoutingBlockChannel channel_two("input");
    TraversalBlockFactory factory_one(1);
    TraversalBlockFactory factory_two(2);
    factory_one.channel = &channel_one;
    factory_two.channel = &channel_two;

    {
        BlockProviderQuerier querier;
        SchedulerPlugin plugin;
        ASSERT_EQ(plugin.Load(&querier), 0);
        for (const std::string& reference : {std::string("pcapfile.missing"), std::string("missing")}) {
            std::shared_ptr<IChannel> owner;
            bool ambiguous = true;
            ASSERT_EQ(SchedulerPluginTestAccessor::FindChannelWithAmbiguity(
                          &plugin, reference, &owner, &ambiguous),
                      nullptr);
            ASSERT_TRUE(!owner);
            ASSERT_TRUE(!ambiguous);
        }
        SqlStatement stmt;
        stmt.sources = {"pcapfile.missing"};
        std::string response;
        ASSERT_EQ(SchedulerPluginTestAccessor::ResolveSourceBindings(&plugin, stmt, &response),
                  error::BAD_REQUEST);
        ASSERT_TRUE(response.find("source channel not found: pcapfile.missing") != std::string::npos);
        ASSERT_EQ(querier.block_factory_first_calls, 0);
    }
    {
        BlockProviderQuerier querier;
        querier.factories = {&factory_one};
        SchedulerPlugin plugin;
        ASSERT_EQ(plugin.Load(&querier), 0);
        for (const std::string& reference : {std::string("pcapfile.input"), std::string("input")}) {
            std::shared_ptr<IChannel> owner;
            bool ambiguous = true;
            ASSERT_EQ(SchedulerPluginTestAccessor::FindChannelWithAmbiguity(
                          &plugin, reference, &owner, &ambiguous),
                      &channel_one);
            ASSERT_EQ(owner.get(), &channel_one);
            ASSERT_TRUE(!ambiguous);
        }
    }
    {
        TraversalBlockFactory unrelated_factory(3);
        TraversalBlockFactory matching_factory(4);
        unrelated_factory.name = "other";
        unrelated_factory.channel = &channel_one;
        matching_factory.channel = &channel_two;
        BlockProviderQuerier querier;
        querier.factories = {&unrelated_factory, &matching_factory};
        SchedulerPlugin plugin;
        ASSERT_EQ(plugin.Load(&querier), 0);
        for (const std::string& reference : {std::string("PCAPFILE.input"), std::string("input")}) {
            std::shared_ptr<IChannel> owner;
            bool ambiguous = true;
            ASSERT_EQ(SchedulerPluginTestAccessor::FindChannelWithAmbiguity(
                          &plugin, reference, &owner, &ambiguous),
                      &channel_two);
            ASSERT_EQ(owner.get(), &channel_two);
            ASSERT_TRUE(!ambiguous);
        }
        ASSERT_EQ(unrelated_factory.get_calls, 1);
        ASSERT_EQ(matching_factory.get_calls, 1);
        ASSERT_EQ(unrelated_factory.list_calls, 1);
        ASSERT_EQ(matching_factory.list_calls, 1);
        ASSERT_EQ(querier.factory_traverse_calls, 2);
        ASSERT_EQ(querier.block_factory_first_calls, 0);
    }
    {
        BlockProviderQuerier querier;
        querier.factories = {&factory_one, &factory_two};
        SchedulerPlugin plugin;
        ASSERT_EQ(plugin.Load(&querier), 0);
        for (const std::string& reference : {std::string("pcapfile.input"), std::string("input")}) {
            std::shared_ptr<IChannel> owner;
            bool ambiguous = false;
            ASSERT_EQ(SchedulerPluginTestAccessor::FindChannelWithAmbiguity(
                          &plugin, reference, &owner, &ambiguous),
                      nullptr);
            ASSERT_TRUE(!owner);
            ASSERT_TRUE(ambiguous);

            SqlStatement stmt;
            stmt.sources = {reference};
            std::string response;
            ASSERT_EQ(SchedulerPluginTestAccessor::ResolveSourceBindings(
                          &plugin, stmt, &response),
                      error::CONFLICT);
            ASSERT_TRUE(response.find("multiple block stream sources matched") != std::string::npos);
        }
        ASSERT_EQ(querier.block_factory_first_calls, 0);
    }

    const std::string add_source =
        R"({"type":"PCAPFILE","name":"input","role":"source","options":{"batch_packets":2}})";
    const std::string modify_source =
        R"({"type":"PCAPFILE","name":"input","role":"source","options":{"batch_packets":3}})";
    const std::string remove = R"({"type":"PCAPFILE","name":"input"})";
    {
        TraversalBlockManager manager(1);
        manager.add_rc = 0;
        BlockProviderQuerier querier;
        querier.managers = {&manager};
        SchedulerPlugin plugin;
        ASSERT_EQ(plugin.Load(&querier), 0);
        for (const char* role : {"sink", "both"}) {
            std::string response;
            const std::string request =
                std::string("{\"type\":\"pcapfile\",\"name\":\"input\",\"role\":\"") + role +
                "\",\"options\":{}}";
            ASSERT_TRUE(SchedulerPluginTestAccessor::HandleAddStreamChannel(
                            &plugin, request, &response) != error::OK);
        }
        ASSERT_EQ(manager.add_calls, 0);
        std::string response;
        ASSERT_EQ(SchedulerPluginTestAccessor::HandleAddStreamChannel(
                      &plugin, add_source, &response),
                  error::OK);
        ASSERT_EQ(manager.add_calls, 1);
        ASSERT_EQ(manager.last_add_type, "pcapfile");
        ASSERT_EQ(manager.last_add_name, "input");
        ASSERT_EQ(manager.last_add_option, R"({"batch_packets":2})");
        ASSERT_EQ(querier.block_manager_first_calls, 0);
    }
    {
        TraversalBlockManager unsupported(1);
        TraversalBlockManager accepted(2);
        accepted.add_rc = 0;
        BlockProviderQuerier querier;
        querier.managers = {&unsupported, &accepted};
        SchedulerPlugin plugin;
        ASSERT_EQ(plugin.Load(&querier), 0);
        std::string response;
        ASSERT_EQ(SchedulerPluginTestAccessor::HandleAddStreamChannel(
                      &plugin, add_source, &response),
                  error::OK);
        ASSERT_EQ(unsupported.add_calls, 1);
        ASSERT_EQ(accepted.add_calls, 1);
        ASSERT_EQ(unsupported.last_add_type, "pcapfile");
        ASSERT_EQ(accepted.last_add_type, "pcapfile");
        ASSERT_EQ(accepted.last_add_option, R"({"batch_packets":2})");
        ASSERT_EQ(querier.block_manager_first_calls, 0);
    }
    {
        TraversalBlockManager first(1);
        TraversalBlockManager second(2);
        first.add_rc = 0;
        second.add_rc = 0;
        BlockProviderQuerier querier;
        querier.managers = {&first, &second};
        SchedulerPlugin plugin;
        ASSERT_EQ(plugin.Load(&querier), 0);
        std::string response;
        ASSERT_EQ(SchedulerPluginTestAccessor::HandleAddStreamChannel(
                      &plugin, add_source, &response),
                  error::CONFLICT);
        ASSERT_EQ(first.add_calls, 1);
        ASSERT_EQ(second.add_calls, 1);
        ASSERT_TRUE(response.find("multiple block stream managers accepted request") != std::string::npos);
        ASSERT_EQ(querier.block_manager_first_calls, 0);
    }
    {
        TraversalBlockManager first_unsupported(1);
        TraversalBlockManager second_unsupported(2);
        BlockProviderQuerier querier;
        querier.managers = {&first_unsupported, &second_unsupported};
        SchedulerPlugin plugin;
        ASSERT_EQ(plugin.Load(&querier), 0);
        std::string response;
        ASSERT_EQ(SchedulerPluginTestAccessor::HandleAddStreamChannel(
                      &plugin, add_source, &response),
                  error::UNAVAILABLE);
        ASSERT_EQ(first_unsupported.add_calls, 1);
        ASSERT_EQ(second_unsupported.add_calls, 1);
        ASSERT_TRUE(response.find("stream manager unavailable") != std::string::npos);
        ASSERT_EQ(querier.block_manager_first_calls, 0);
    }
    {
        TraversalBlockManager owner(1);
        TraversalBlockManager absent(2);
        owner.owns_channel = true;
        owner.modify_rc = 0;
        owner.remove_rc = 0;
        BlockProviderQuerier querier;
        querier.managers = {&owner, &absent};
        SchedulerPlugin plugin;
        ASSERT_EQ(plugin.Load(&querier), 0);
        std::string response;
        ASSERT_EQ(SchedulerPluginTestAccessor::HandleModifyStreamChannel(
                      &plugin, modify_source, &response),
                  error::OK);
        ASSERT_EQ(owner.modify_calls, 1);
        ASSERT_EQ(absent.modify_calls, 0);
        ASSERT_EQ(owner.last_modify_type, "pcapfile");
        ASSERT_EQ(owner.last_modify_name, "input");
        ASSERT_EQ(owner.last_modify_option, R"({"batch_packets":3})");
        ASSERT_EQ(SchedulerPluginTestAccessor::HandleRemoveStreamChannel(
                      &plugin, remove, &response),
                  error::OK);
        ASSERT_EQ(owner.remove_calls, 1);
        ASSERT_EQ(absent.remove_calls, 0);
        ASSERT_EQ(owner.last_remove_type, "pcapfile");
        ASSERT_EQ(owner.last_remove_name, "input");
        ASSERT_EQ(owner.query_calls, 2);
        ASSERT_EQ(absent.query_calls, 2);
        ASSERT_EQ(querier.block_manager_first_calls, 0);
    }
    for (const char* role : {"sink", "both"}) {
        TraversalBlockManager owner(1);
        owner.owns_channel = true;
        owner.modify_rc = 0;
        BlockProviderQuerier querier;
        querier.managers = {&owner};
        SchedulerPlugin plugin;
        ASSERT_EQ(plugin.Load(&querier), 0);
        std::string response;
        const std::string request =
            std::string("{\"type\":\"pcapfile\",\"name\":\"input\",\"role\":\"") + role +
            "\",\"options\":{}}";
        ASSERT_EQ(SchedulerPluginTestAccessor::HandleModifyStreamChannel(
                      &plugin, request, &response),
                  error::BAD_REQUEST);
        ASSERT_EQ(owner.modify_calls, 0);
    }
    {
        TraversalBlockManager first_owner(1);
        TraversalBlockManager second_owner(2);
        first_owner.owns_channel = true;
        second_owner.owns_channel = true;
        first_owner.modify_rc = 0;
        second_owner.modify_rc = 0;
        first_owner.remove_rc = 0;
        second_owner.remove_rc = 0;
        BlockProviderQuerier querier;
        querier.managers = {&first_owner, &second_owner};
        SchedulerPlugin plugin;
        ASSERT_EQ(plugin.Load(&querier), 0);
        std::string response;
        ASSERT_EQ(SchedulerPluginTestAccessor::HandleModifyStreamChannel(
                      &plugin, modify_source, &response),
                  error::CONFLICT);
        ASSERT_EQ(first_owner.modify_calls, 0);
        ASSERT_EQ(second_owner.modify_calls, 0);
        ASSERT_TRUE(response.find("multiple block stream managers own channel") != std::string::npos);
        ASSERT_EQ(SchedulerPluginTestAccessor::HandleRemoveStreamChannel(
                      &plugin, remove, &response),
                  error::CONFLICT);
        ASSERT_EQ(first_owner.remove_calls, 0);
        ASSERT_EQ(second_owner.remove_calls, 0);
        ASSERT_TRUE(response.find("multiple block stream managers own channel") != std::string::npos);
        ASSERT_EQ(first_owner.query_calls, 2);
        ASSERT_EQ(second_owner.query_calls, 2);
        ASSERT_EQ(querier.block_manager_first_calls, 0);
    }
    {
        TraversalBlockManager first_absent(1);
        TraversalBlockManager second_absent(2);
        BlockProviderQuerier querier;
        querier.managers = {&first_absent, &second_absent};
        SchedulerPlugin plugin;
        ASSERT_EQ(plugin.Load(&querier), 0);
        std::string response;
        ASSERT_EQ(SchedulerPluginTestAccessor::HandleModifyStreamChannel(
                      &plugin, modify_source, &response),
                  error::NOT_FOUND);
        ASSERT_EQ(first_absent.modify_calls, 1);
        ASSERT_EQ(second_absent.modify_calls, 1);
        ASSERT_EQ(SchedulerPluginTestAccessor::HandleRemoveStreamChannel(
                      &plugin, remove, &response),
                  error::NOT_FOUND);
        ASSERT_EQ(first_absent.remove_calls, 1);
        ASSERT_EQ(second_absent.remove_calls, 1);
        ASSERT_TRUE(response.find("block stream channel not found") != std::string::npos);
        ASSERT_EQ(first_absent.query_calls, 2);
        ASSERT_EQ(second_absent.query_calls, 2);
        ASSERT_EQ(querier.block_manager_first_calls, 0);
    }
}

int32_t ExecuteBlockRoute(SchemaBlockChannel* source,
                          SchemaBlockOperator* op,
                          std::string* response) {
    TraversalBlockFactory factory(1);
    factory.channel = source;
    BlockProviderQuerier querier;
    querier.factories = {&factory};
    querier.operators = {op};
    SchedulerPlugin plugin;
    ASSERT_EQ(plugin.Load(&querier), 0);
    return SchedulerPluginTestAccessor::HandleExecute(
        &plugin,
        R"({"sql":"SELECT * FROM pcapfile.input USING test.schema_guard"})",
        response);
}

BatchRuntimeSnapshot WaitForBatchTerminal(SchedulerPlugin* plugin,
                                          const std::string& runtime_task_id) {
    BatchRuntimeSnapshot snapshot;
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (SchedulerPluginTestAccessor::QueryBatchSnapshot(
                plugin, runtime_task_id, &snapshot) == 0 &&
            IsTerminalBatchRuntimeStatus(snapshot.status)) {
            return snapshot;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(false);
    return snapshot;
}

BatchRuntimeSnapshot RunBlockBatchCase(SchemaBlockChannel* source,
                                       SchemaBlockOperator* op,
                                       const std::string& runtime_task_id) {
    TraversalBlockFactory factory(1);
    factory.channel = source;
    BlockProviderQuerier querier;
    querier.factories = {&factory};
    querier.operators = {op};
    SchedulerPlugin plugin;
    ASSERT_EQ(plugin.Load(&querier), 0);
    ASSERT_EQ(SchedulerPluginTestAccessor::StartBatchRuntime(&plugin), 0);

    const std::string request =
        "{\"runtime_task_id\":\"" + runtime_task_id +
        "\",\"sqls\":[\"SELECT * FROM pcapfile.input USING test.schema_guard\"]}";
    std::string response;
    ASSERT_EQ(SchedulerPluginTestAccessor::HandleBatchSubmit(
                  &plugin, request, &response),
              error::OK);
    ASSERT_TRUE(response.find("\"status\":\"submitted\"") != std::string::npos);

    BatchRuntimeSnapshot snapshot = WaitForBatchTerminal(&plugin, runtime_task_id);
    SchedulerPluginTestAccessor::StopBatchRuntime(&plugin);
    return snapshot;
}

void TestBlockOperatorSchemaGate() {
    const std::string request =
        R"({"sql":"SELECT * FROM pcapfile.input USING test.schema_guard"})";
    auto execute = [&](const std::shared_ptr<arrow::Schema>& schema,
                       int schema_rc,
                       int data_count,
                       SchemaBlockOperator* op,
                       std::string* response) {
        auto batch = arrow::RecordBatch::Make(
            schema, 0, std::vector<std::shared_ptr<arrow::Array>>{});
        SchemaBlockChannel source(batch, data_count);
        op->schema_rc = schema_rc;
        TraversalBlockFactory factory(1);
        factory.channel = &source;
        BlockProviderQuerier querier;
        querier.factories = {&factory};
        querier.operators = {op};
        SchedulerPlugin plugin;
        ASSERT_EQ(plugin.Load(&querier), 0);
        return SchedulerPluginTestAccessor::HandleExecute(&plugin, request, response);
    };

    {
        SchemaBlockOperator op;
        op.process_stop_after = 2;
        std::string response;
        ASSERT_EQ(execute(packet::PacketSchema(), 0, 2, &op, &response), error::OK);
        ASSERT_TRUE(response.find("BLOCK_STREAM_NOT_IMPLEMENTED") == std::string::npos);
        ASSERT_EQ(op.init_calls, 1);
        ASSERT_EQ(op.schema_calls, 1);
        ASSERT_EQ(op.process_calls, 2);
        ASSERT_TRUE(op.schema_before_process);
        ASSERT_TRUE(op.schema_matches_packet);
    }
    {
        SchemaBlockOperator op;
        std::string response;
        ASSERT_EQ(execute(arrow::schema({}), 0, 1, &op, &response),
                  error::INTERNAL_ERROR);
        ASSERT_EQ(op.schema_calls, 0);
        ASSERT_EQ(op.process_calls, 0);
    }
    for (const int schema_rc : {EINVAL, -EIO}) {
        SchemaBlockOperator op;
        std::string response;
        ASSERT_EQ(execute(packet::PacketSchema(), schema_rc, 1, &op, &response),
                  error::INTERNAL_ERROR);
        ASSERT_EQ(op.schema_calls, 1);
        ASSERT_EQ(op.process_calls, 0);
    }
}

void TestBlockOperatorPollAndRelease() {
    auto make_batch = []() {
        return arrow::RecordBatch::Make(
            packet::PacketSchema(), 0, std::vector<std::shared_ptr<arrow::Array>>{});
    };
    SchedulerPlugin plugin;

    {
        SchemaBlockChannel source(make_batch(), 1);
        source.timeout_count = 2;
        SchemaBlockOperator op;
        op.process_stop_after = 2;
        int64_t rows = -1;
        std::string error;
        ASSERT_EQ(SchedulerPluginTestAccessor::ExecuteBlockOperator(
                      &plugin, &source, &op, &rows, &error),
                  0);
        ASSERT_EQ(source.poll_calls, 4);
        ASSERT_EQ(source.release_calls, 1);
        ASSERT_EQ(op.process_calls, 1);
        ASSERT_EQ(op.flush_calls, 1);
        ASSERT_EQ(rows, 0);
    }
    {
        SchemaBlockChannel source(make_batch(), 0);
        SchemaBlockOperator op;
        int64_t rows = -1;
        std::string error;
        ASSERT_EQ(SchedulerPluginTestAccessor::ExecuteBlockOperator(
                      &plugin, &source, &op, &rows, &error),
                  0);
        ASSERT_EQ(source.poll_calls, 1);
        ASSERT_EQ(source.release_calls, 0);
        ASSERT_EQ(op.schema_calls, 0);
        ASSERT_EQ(op.process_calls, 0);
        ASSERT_EQ(op.flush_calls, 1);
        ASSERT_EQ(rows, 0);
    }
    for (const int event_error : {0, EINTR}) {
        SchemaBlockChannel source(make_batch(), 0);
        source.terminal_kind = BlockPollEvent::kCancelled;
        source.terminal_err = event_error;
        SchemaBlockOperator op;
        std::string error;
        ASSERT_EQ(SchedulerPluginTestAccessor::ExecuteBlockOperator(
                      &plugin, &source, &op, nullptr, &error),
                  event_error == 0 ? ECANCELED : event_error);
        ASSERT_EQ(source.release_calls, 0);
        ASSERT_EQ(op.flush_calls, 0);
    }
    for (const int event_error : {0, EPROTO}) {
        SchemaBlockChannel source(make_batch(), 0);
        source.terminal_kind = BlockPollEvent::kError;
        source.terminal_err = event_error;
        SchemaBlockOperator op;
        std::string error;
        ASSERT_EQ(SchedulerPluginTestAccessor::ExecuteBlockOperator(
                      &plugin, &source, &op, nullptr, &error),
                  event_error == 0 ? EIO : event_error);
        ASSERT_EQ(source.release_calls, 0);
        ASSERT_EQ(op.flush_calls, 0);
    }
    {
        SchemaBlockChannel source(make_batch(), 2);
        SchemaBlockOperator op;
        op.process_stop_after = 2;
        std::string error;
        ASSERT_EQ(SchedulerPluginTestAccessor::ExecuteBlockOperator(
                      &plugin, &source, &op, nullptr, &error),
                  0);
        ASSERT_EQ(source.poll_calls, 2);
        ASSERT_EQ(source.release_calls, 2);
        ASSERT_EQ(op.process_calls, 2);
        ASSERT_EQ(op.flush_calls, 1);
    }
    {
        SchemaBlockChannel source(make_batch(), 1);
        SchemaBlockOperator op;
        op.process_rc = -EIO;
        std::string error;
        ASSERT_EQ(SchedulerPluginTestAccessor::ExecuteBlockOperator(
                      &plugin, &source, &op, nullptr, &error),
                  -EIO);
        ASSERT_EQ(source.release_calls, 1);
        ASSERT_EQ(op.process_calls, 1);
        ASSERT_EQ(op.flush_calls, 0);
    }
    {
        SchemaBlockChannel source(make_batch(), 2);
        SchemaBlockOperator op;
        op.process_rc = 2;
        std::string error;
        ASSERT_EQ(SchedulerPluginTestAccessor::ExecuteBlockOperator(
                      &plugin, &source, &op, nullptr, &error),
                  2);
        ASSERT_EQ(source.poll_calls, 1);
        ASSERT_EQ(source.release_calls, 1);
        ASSERT_EQ(op.process_calls, 1);
        ASSERT_EQ(op.flush_calls, 0);
    }
    {
        SchemaBlockChannel source(make_batch(), 1);
        source.release_rc = EBUSY;
        SchemaBlockOperator op;
        op.process_stop_after = 2;
        std::string error;
        ASSERT_EQ(SchedulerPluginTestAccessor::ExecuteBlockOperator(
                      &plugin, &source, &op, nullptr, &error),
                  EBUSY);
        ASSERT_EQ(source.release_calls, 1);
        ASSERT_EQ(op.process_calls, 1);
        ASSERT_EQ(op.flush_calls, 0);
    }
}

void TestBlockTerminalRouteAndBatchSnapshots() {
    auto make_batch = []() {
        return arrow::RecordBatch::Make(
            packet::PacketSchema(), 0, std::vector<std::shared_ptr<arrow::Array>>{});
    };

    {
        SchemaBlockChannel source(make_batch(), 1);
        SchemaBlockOperator op;
        op.process_stop_after = 2;
        std::string response;
        ASSERT_EQ(ExecuteBlockRoute(&source, &op, &response), error::OK);
        ASSERT_TRUE(response.find("\"status\":\"completed\"") != std::string::npos);
        ASSERT_EQ(source.release_calls, 1);
        ASSERT_EQ(op.flush_calls, 1);
    }
    {
        SchemaBlockChannel source(make_batch(), 2);
        SchemaBlockOperator op;
        std::string response;
        ASSERT_EQ(ExecuteBlockRoute(&source, &op, &response), error::OK);
        ASSERT_TRUE(response.find("\"status\":\"stopped\"") != std::string::npos);
        ASSERT_EQ(source.poll_calls, 1);
        ASSERT_EQ(source.release_calls, 1);
        ASSERT_EQ(op.flush_calls, 1);
    }
    {
        SchemaBlockChannel source(make_batch(), 0);
        source.terminal_kind = BlockPollEvent::kCancelled;
        source.terminal_err = EINTR;
        SchemaBlockOperator op;
        std::string response;
        ASSERT_EQ(ExecuteBlockRoute(&source, &op, &response), error::OK);
        ASSERT_TRUE(response.find("\"status\":\"cancelled\"") != std::string::npos);
        ASSERT_EQ(source.release_calls, 0);
        ASSERT_EQ(op.flush_calls, 0);
    }
    {
        SchemaBlockChannel source(make_batch(), 2);
        SchemaBlockOperator op;
        op.throw_on_process = true;
        std::string response;
        ASSERT_EQ(ExecuteBlockRoute(&source, &op, &response), error::INTERNAL_ERROR);
        ASSERT_TRUE(response.find("\"error_code\":\"OP_EXEC_FAIL\"") != std::string::npos);
        ASSERT_TRUE(response.find("\"error_stage\":\"execute\"") != std::string::npos);
        ASSERT_EQ(source.poll_calls, 1);
        ASSERT_EQ(source.release_calls, 1);
        ASSERT_EQ(op.flush_calls, 0);
    }
    {
        SchemaBlockChannel source(make_batch(), 2);
        SchemaBlockOperator op;
        op.process_rc = -EIO;
        std::string response;
        ASSERT_EQ(ExecuteBlockRoute(&source, &op, &response), error::INTERNAL_ERROR);
        ASSERT_TRUE(response.find("\"error_code\":\"OP_EXEC_FAIL\"") != std::string::npos);
        ASSERT_TRUE(response.find("\"error_stage\":\"execute\"") != std::string::npos);
        ASSERT_EQ(source.poll_calls, 1);
        ASSERT_EQ(source.release_calls, 1);
        ASSERT_EQ(op.flush_calls, 0);
    }

    {
        SchemaBlockChannel source(make_batch(), 1);
        SchemaBlockOperator op;
        op.process_stop_after = 2;
        const BatchRuntimeSnapshot snapshot =
            RunBlockBatchCase(&source, &op, "block_terminal_completed");
        ASSERT_EQ(snapshot.status, BatchRuntimeStatus::kCompleted);
        ASSERT_TRUE(snapshot.finished_ms > 0);
        ASSERT_EQ(snapshot.last_active_ms, snapshot.finished_ms);
        ASSERT_EQ(source.release_calls, 1);
        ASSERT_EQ(op.flush_calls, 1);
    }
    {
        SchemaBlockChannel source(make_batch(), 2);
        SchemaBlockOperator op;
        const BatchRuntimeSnapshot snapshot =
            RunBlockBatchCase(&source, &op, "block_terminal_stopped");
        ASSERT_EQ(snapshot.status, BatchRuntimeStatus::kStopped);
        ASSERT_TRUE(snapshot.finished_ms > 0);
        ASSERT_EQ(snapshot.last_active_ms, snapshot.finished_ms);
        ASSERT_EQ(source.poll_calls, 1);
        ASSERT_EQ(source.release_calls, 1);
        ASSERT_EQ(op.flush_calls, 1);
    }
    {
        SchemaBlockChannel source(make_batch(), 0);
        source.terminal_kind = BlockPollEvent::kCancelled;
        source.terminal_err = EINTR;
        SchemaBlockOperator op;
        const BatchRuntimeSnapshot snapshot =
            RunBlockBatchCase(&source, &op, "block_terminal_cancelled");
        ASSERT_EQ(snapshot.status, BatchRuntimeStatus::kCancelled);
        ASSERT_TRUE(snapshot.finished_ms > 0);
        ASSERT_EQ(snapshot.last_active_ms, snapshot.finished_ms);
        ASSERT_EQ(source.release_calls, 0);
        ASSERT_EQ(op.flush_calls, 0);
    }
    {
        SchemaBlockChannel source(make_batch(), 2);
        SchemaBlockOperator op;
        op.throw_on_process = true;
        const BatchRuntimeSnapshot snapshot =
            RunBlockBatchCase(&source, &op, "block_terminal_exception");
        ASSERT_EQ(snapshot.status, BatchRuntimeStatus::kFailed);
        ASSERT_EQ(snapshot.error_code, "OP_EXEC_FAIL");
        ASSERT_EQ(snapshot.error_stage, "execute");
        ASSERT_TRUE(snapshot.finished_ms > 0);
        ASSERT_EQ(snapshot.last_active_ms, snapshot.finished_ms);
        ASSERT_EQ(source.poll_calls, 1);
        ASSERT_EQ(source.release_calls, 1);
        ASSERT_EQ(op.flush_calls, 0);
    }
    {
        SchemaBlockChannel source(make_batch(), 2);
        SchemaBlockOperator op;
        op.process_rc = -EIO;
        const BatchRuntimeSnapshot snapshot =
            RunBlockBatchCase(&source, &op, "block_terminal_operator_error");
        ASSERT_EQ(snapshot.status, BatchRuntimeStatus::kFailed);
        ASSERT_EQ(snapshot.error_code, "OP_EXEC_FAIL");
        ASSERT_EQ(snapshot.error_stage, "execute");
        ASSERT_TRUE(snapshot.finished_ms > 0);
        ASSERT_EQ(snapshot.last_active_ms, snapshot.finished_ms);
        ASSERT_EQ(source.poll_calls, 1);
        ASSERT_EQ(source.release_calls, 1);
        ASSERT_EQ(op.flush_calls, 0);
    }
}

void TestBlockDirectTransferTimeout() {
    SchemaBlockChannel source(nullptr, 0);
    source.timeout_count = 2;
    TraversalBlockFactory factory(1);
    factory.channel = &source;
    BlockProviderQuerier querier;
    querier.factories = {&factory};
    SchedulerPlugin plugin;
    ASSERT_EQ(plugin.Load(&querier), 0);
    std::string response;
    ASSERT_EQ(SchedulerPluginTestAccessor::HandleExecute(
                  &plugin,
                  R"({"sql":"SELECT * FROM pcapfile.input"})",
                  &response),
              error::OK);
    ASSERT_EQ(source.poll_calls, 3);
    ASSERT_EQ(source.release_calls, 0);
}
}  // namespace scheduler
}  // namespace flowsql

int main() {
    std::puts("=== Scheduler mutation guard tests ===");

    flowsql::scheduler::TestBlockProviderTraversal();
    flowsql::scheduler::TestBlockSourceAndManagerRouting();
    flowsql::scheduler::TestBlockOperatorSchemaGate();
    flowsql::scheduler::TestBlockOperatorPollAndRelease();
    flowsql::scheduler::TestBlockTerminalRouteAndBatchSnapshots();
    flowsql::scheduler::TestBlockDirectTransferTimeout();

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

    // SharedSourceHub concurrent AddSubscriber must honor max_subscribers hard cap.
    {
        flowsql::RingStreamChannelOptions source_opts;
        source_opts.ring_size = 64;
        source_opts.ring_mode = flowsql::RingMode::SPSC;
        source_opts.overflow = flowsql::OverflowPolicy::kDrop;
        auto source = std::make_shared<flowsql::RingStreamChannel>("ring", "shared_cap_src", source_opts);
        ASSERT_EQ(source->Open(), 0);

        flowsql::scheduler::SharedHubOptions hub_opts;
        hub_opts.queue_size = 64;
        hub_opts.poll_timeout_ms = 10;
        hub_opts.overflow_policy = flowsql::OverflowPolicy::kDrop;
        hub_opts.ring_mode = flowsql::RingMode::SPSC;
        auto hub = std::make_shared<flowsql::scheduler::SharedSourceHub>(
            "shared_cap_hub",
            flowsql::scheduler::SharedHubMode::kDynamic,
            "ring.shared_cap_src",
            std::vector<std::string>{"ring.shared_cap_src"},
            source,
            hub_opts);

        constexpr int kThreads = 16;
        std::atomic<int> success_count{0};
        std::atomic<int> busy_count{0};
        std::atomic<int> unexpected_rc{0};
        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (int i = 0; i < kThreads; ++i) {
            workers.emplace_back([i, &hub, &success_count, &busy_count, &unexpected_rc]() {
                std::string err_msg;
                const int rc = hub->AddSubscriber(
                    "runtime_sub_" + std::to_string(i),
                    "",
                    true,
                    nullptr,
                    &err_msg,
                    1);
                if (rc == 0) {
                    success_count.fetch_add(1, std::memory_order_relaxed);
                } else if (rc == EBUSY) {
                    busy_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    unexpected_rc.store(rc, std::memory_order_release);
                }
            });
        }
        for (auto& th : workers) th.join();

        ASSERT_EQ(unexpected_rc.load(std::memory_order_acquire), 0);
        ASSERT_EQ(success_count.load(std::memory_order_relaxed), 1);
        ASSERT_TRUE(busy_count.load(std::memory_order_relaxed) >= (kThreads - 1));
        ASSERT_EQ(hub->SubscriberCount(), static_cast<size_t>(1));

        hub->RequestStop();
        hub->Join();
        source->Close();
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
