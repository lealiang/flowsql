/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_SCHEDULER_SCHEDULER_STREAM_GROUP_INTERNAL_H_
#define _FLOWSQL_SCHEDULER_SCHEDULER_STREAM_GROUP_INTERNAL_H_

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <framework/interfaces/istream_channel.h>

#include "shared_source_hub.h"
#include "stream_task_group.h"

namespace flowsql {
namespace scheduler {

struct StreamGroupExecuteRequest {
    int timeout_s = 0;
    int share_set_ready_timeout_s = 30;
    std::vector<std::string> sqls;
};

struct StreamGroupNodeResolvedMeta {
    std::vector<std::string> source_keys;
    std::vector<std::string> resolved_sources;
    std::string expand_rule = "explicit";
    std::vector<std::shared_ptr<IStreamChannel>> stream_channels;
    bool has_stream_source = false;
    bool has_non_stream_source = false;
};

struct StreamGroupShareSetPlan {
    std::string id;
    std::string source_ref;
    std::vector<std::string> members;
    std::vector<std::string> canonical_source_keys;
    std::vector<std::shared_ptr<IStreamChannel>> source_channels;
};

struct StreamGroupBuildArtifacts {
    std::vector<GroupNodePlan> plans;
    std::unordered_map<std::string, size_t> node_index;
    std::unordered_map<std::string, StreamGroupNodeResolvedMeta> node_resolved;
    std::vector<std::string> group_source_keys;
    std::vector<std::string> group_sink_keys;
    std::vector<StreamGroupShareSetPlan> share_set_plans;
};

struct StreamGroupShareSetRuntimeItem {
    std::string id;
    std::string source_ref;
    std::vector<std::string> members;
    std::vector<std::string> internal_channel_refs;
    std::shared_ptr<SharedSourceHub> hub;
};

struct StreamGroupRuntimeArtifacts {
    std::unordered_map<std::string, std::string> node_source_overrides;
    std::vector<StreamGroupShareSetRuntimeItem> share_set_runtimes;
    std::vector<std::string> created_channel_refs;
};

struct StreamGroupCallbackContext {
    std::string group_runtime_task_id;
    int timeout_s = 0;
    std::unordered_map<std::string, GroupNodeKind> node_kind_by_id;
    std::unordered_map<std::string, std::string> node_source_overrides;
    std::unordered_map<std::string, GroupNodeKind> runtime_kind_by_node_task_id;
    std::mutex runtime_kind_mu;
};

}  // namespace scheduler
}  // namespace flowsql

#endif  // _FLOWSQL_SCHEDULER_SCHEDULER_STREAM_GROUP_INTERNAL_H_
