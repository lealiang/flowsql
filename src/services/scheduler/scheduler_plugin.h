#ifndef _FLOWSQL_SCHEDULER_SCHEDULER_PLUGIN_H_
#define _FLOWSQL_SCHEDULER_SCHEDULER_PLUGIN_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <common/error_code.h>
#include <common/iplugin.h>
#include <common/span.h>
#include <framework/interfaces/irouter_handle.h>
#include <framework/interfaces/ibridge.h>
#include <framework/interfaces/istream_channel.h>

#include <rapidjson/document.h>

#include "stream_runtime.h"
#include "broadcast_hub.h"
#include "stream_task_group.h"

namespace flowsql {

class IChannel;
class IOperator;
struct SqlStatement;

namespace scheduler {

struct GroupNodeResolvedSourceMeta {
    std::vector<std::string> sources;
    std::string expand_rule = "explicit";
};

// SchedulerPlugin — SQL 执行调度插件
// 通过 IRouterHandle 声明路由，对 HTTP 完全无感知
class SchedulerPlugin : public IPlugin, public IRouterHandle {
 public:
    SchedulerPlugin() = default;
    ~SchedulerPlugin() override = default;

    // IPlugin
    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;

    // IRouterHandle — 声明路由
    void EnumRoutes(std::function<void(const RouteItem&)> callback) override;

 private:
    friend struct SchedulerPluginTestAccessor;

    // 路由处理（fnRouterHandler 签名）
    int32_t HandleExecute(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleSqlClassify(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleStreamExecute(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleStreamStop(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleStreamStatus(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleStreamList(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleGetChannels(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleQueryStreamChannelDefinitions(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleQueryStreamChannels(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleAddStreamChannel(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleModifyStreamChannel(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleRemoveStreamChannel(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleRefreshOperators(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandlePreviewDataframe(const std::string& uri, const std::string& req, std::string& rsp);

    // 通道管理
    void RegisterManagedChannel(const std::string& key, std::shared_ptr<IChannel> ch);
    void EraseManagedChannel(const std::string& key);
    void ClearManagedChannels();
    std::shared_ptr<IChannel> FindManagedChannelShared(const std::string& key);
    std::vector<std::pair<std::string, std::shared_ptr<IChannel>>> SnapshotManagedChannels();
    IChannel* FindChannel(const std::string& name);
    IChannel* FindChannel(const std::string& name, std::shared_ptr<IChannel>* owner_out);
    void RegisterChannel(const std::string& key, std::shared_ptr<IChannel> ch);

    // 算子查找
    std::shared_ptr<IOperator> FindOperator(const std::string& category, const std::string& name);
    std::shared_ptr<IOperator> CreateOperator(const std::string& category, const std::string& name);

    // 执行路径
    int ExecuteTransfer(IChannel* source, IChannel* sink,
                        const std::string& source_type, const std::string& sink_type,
                        const SqlStatement& stmt, int64_t* rows_affected = nullptr,
                        std::string* error = nullptr);

    int ExecuteWithOperator(IChannel* source, IChannel* sink, IOperator* op,
                            const std::string& sink_type,
                            const SqlStatement& stmt, int64_t* rows_affected = nullptr,
                            std::string* error = nullptr);
    int ExecuteWithOperatorChain(Span<IChannel*> inputs, IChannel* sink, const std::vector<IOperator*>& ops,
                                 const std::string& sink_type,
                                 const SqlStatement& stmt, int64_t* rows_affected = nullptr,
                                 std::string* error = nullptr);

    int32_t ExecuteStreamTask(const SqlStatement& stmt,
                              std::string& rsp,
                              const std::string& lease_owner_id = "",
                              bool skip_lease_acquire = false);
    int32_t HandleStreamExecuteSingle(const rapidjson::Document& doc, std::string& rsp);
    int32_t HandleStreamExecuteGroup(const rapidjson::Document& doc, std::string& rsp);
    int32_t ClassifySqlTaskKind(const std::string& sql_text, std::string* task_kind, std::string* err_rsp);
    int QueryStreamTaskSnapshotByRuntimeId(const std::string& runtime_task_id, TaskSnapshot* snapshot_out);
    void RequestStopStreamTaskByRuntimeId(const std::string& runtime_task_id);
    std::vector<BroadcastHubSnapshot> QueryGroupShareSetSnapshots(const std::string& group_runtime_task_id);
    std::unordered_map<std::string, GroupNodeResolvedSourceMeta> QueryGroupNodeResolvedSources(
        const std::string& group_runtime_task_id);
    void CleanupGroupRuntimeResources(const std::string& group_runtime_task_id,
                                      const StreamGroupSnapshot* group_snapshot = nullptr);
    std::string NextStreamTaskId();

    struct SinkBinding {
        std::shared_ptr<IChannel> sink_channel;
        std::string sink_type;
        std::string db_type;
        std::string db_name;
        std::string table_name;
    };

    int32_t ResolveStreamSink(const SqlStatement& stmt,
                              SinkBinding* binding,
                              std::string* err_out);
    struct SourceResolveResult {
        std::vector<IChannel*> channels;
        std::vector<std::shared_ptr<IChannel>> channel_holders;
        std::vector<std::shared_ptr<IStreamChannel>> stream_channels;
        std::vector<std::string> source_keys;
        std::vector<std::string> resolved_sources;
        std::string source_expand_rule = "explicit";
        bool has_stream_source = false;
        bool has_non_stream_source = false;
    };
    int32_t ResolveSourceBindings(const SqlStatement& stmt,
                                  SourceResolveResult* out,
                                  std::string* err_rsp);
    std::string QueryStreamChannelRole(const std::string& type, const std::string& name);
    int TryAcquireStreamTaskLeases(const std::string& runtime_task_id,
                                   const std::vector<std::string>& source_keys,
                                   const std::vector<std::string>& sink_keys,
                                   std::string* conflict_key_out,
                                   bool* blocked_by_mutation_out = nullptr,
                                   const std::string& lease_owner_id = "",
                                   const std::unordered_map<std::string, uint64_t>* expected_versions = nullptr,
                                   std::string* version_conflict_key_out = nullptr);
    void CaptureStreamChannelVersionSnapshot(
        const std::vector<std::string>& keys,
        std::unordered_map<std::string, uint64_t>* snapshot_out);
    int TryBeginStreamChannelMutation(const std::string& key, std::string* reason_out);
    void EndStreamChannelMutation(const std::string& key);
    void ReleaseStreamTaskLeases(const std::string& runtime_task_id);
    void SweepFinishedTaskLeases();

    IQuerier* querier_ = nullptr;

    // 通道表
    mutable std::mutex channels_mu_;
    std::unordered_map<std::string, std::shared_ptr<IChannel>> channels_;

    std::string host_ = "127.0.0.1";
    int port_ = 18803;
    size_t max_resolved_sources_ = 64;
    int max_stream_group_timeout_s_ = 86400;

    // 用于生成唯一临时通道名
    std::atomic<uint64_t> tmp_channel_seq_{0};

    // 流式任务调度
    size_t stream_worker_count_ = 0;
    StreamRuntime stream_runtime_;
    std::atomic<uint64_t> stream_task_seq_{0};
    mutable std::mutex stream_tasks_mu_;
    std::unordered_map<std::string, std::shared_ptr<StreamTask>> stream_tasks_;
    mutable std::mutex stream_task_groups_mu_;
    std::unordered_map<std::string, std::shared_ptr<StreamTaskGroup>> stream_task_groups_;
    mutable std::mutex stream_group_nodes_mu_;
    std::unordered_map<std::string, std::string> stream_group_node_owners_;
    mutable std::mutex stream_group_node_sources_mu_;
    std::unordered_map<std::string, std::unordered_map<std::string, GroupNodeResolvedSourceMeta>>
        stream_group_node_sources_;
    struct StreamGroupShareSetRuntime {
        std::string id;
        std::string source_ref;
        std::vector<std::string> members;
        std::vector<std::string> internal_channel_refs;
        std::shared_ptr<BroadcastHub> hub;
    };
    mutable std::mutex stream_group_share_sets_mu_;
    std::unordered_map<std::string, std::vector<StreamGroupShareSetRuntime>> stream_group_share_sets_;
    mutable std::mutex stream_group_share_set_snapshots_mu_;
    std::unordered_map<std::string, std::vector<BroadcastHubSnapshot>> stream_group_share_set_snapshots_;

    struct StreamTaskLeaseInfo {
        std::vector<std::string> all_keys;
        std::vector<std::string> source_keys;
        std::string lease_owner_id;
    };
    struct StreamSourceLeaseState {
        std::string owner_id;
        uint32_t ref_count = 0;
    };
    std::mutex stream_channel_refs_mu_;
    std::unordered_map<std::string, uint32_t> stream_channel_ref_counts_;
    std::unordered_map<std::string, uint64_t> stream_channel_versions_;
    std::unordered_map<std::string, StreamSourceLeaseState> stream_source_leases_;
    std::unordered_set<std::string> stream_channel_mutating_;
    std::unordered_map<std::string, StreamTaskLeaseInfo> stream_task_leases_;
};

}  // namespace scheduler
}  // namespace flowsql

#endif  // _FLOWSQL_SCHEDULER_SCHEDULER_PLUGIN_H_
