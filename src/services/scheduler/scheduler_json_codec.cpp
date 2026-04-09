/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "scheduler_json_codec.h"
#include "scheduler_plugin.h"

#include <rapidjson/document.h>

namespace flowsql {
namespace scheduler {

const char* StreamTaskStatusName(StreamTaskStatus status) {
    switch (status) {
        case StreamTaskStatus::kCreated: return "created";
        case StreamTaskStatus::kRunning: return "running";
        case StreamTaskStatus::kStopping: return "stopping";
        case StreamTaskStatus::kStopped: return "stopped";
        case StreamTaskStatus::kCancelled: return "cancelled";
        case StreamTaskStatus::kFailed: return "failed";
        default: return "unknown";
    }
}

bool IsTerminalStreamTaskStatus(StreamTaskStatus status) {
    return status == StreamTaskStatus::kStopped ||
           status == StreamTaskStatus::kCancelled ||
           status == StreamTaskStatus::kFailed;
}

void WriteTaskSnapshotJson(rapidjson::Writer<rapidjson::StringBuffer>* w,
                           const TaskSnapshot& s,
                           const SharedHubSnapshot* shared_hub) {
    if (!w) return;
    w->StartObject();
    w->Key("task_id");
    w->String(s.task_id.c_str());
    w->Key("runtime_task_id");
    w->String(s.task_id.c_str());
    w->Key("runtime_kind");
    w->String("single");
    w->Key("status");
    w->String(StreamTaskStatusName(s.status));
    w->Key("stop_requested");
    w->Bool(s.stop_requested);
    w->Key("joined");
    w->Bool(s.joined);
    w->Key("shard_count");
    w->Uint(s.shard_count);
    w->Key("active_shards");
    w->Uint(s.active_shards);

    w->Key("processed_batches");
    w->Uint64(s.processed_batches);
    w->Key("processed_rows");
    w->Uint64(s.processed_rows);
    w->Key("processed_bytes");
    w->Uint64(s.processed_bytes);
    w->Key("output_rows");
    w->Uint64(s.output_rows);
    w->Key("output_batches");
    w->Uint64(s.output_batches);
    w->Key("dropped_batches");
    w->Uint64(s.dropped_batches);
    w->Key("poll_timeouts");
    w->Uint64(s.poll_timeouts);
    w->Key("poll_errors");
    w->Uint64(s.poll_errors);
    w->Key("queue_depth");
    w->Uint64(s.queue_depth);
    w->Key("queue_depth_peak");
    w->Uint64(s.queue_depth_peak);
    w->Key("uptime_ms");
    w->Int64(s.uptime_ms);
    w->Key("started_ms");
    w->Int64(s.started_ms);
    w->Key("last_active_ms");
    w->Int64(s.last_active_ms);
    w->Key("finished_ms");
    w->Int64(s.finished_ms);
    w->Key("error_code");
    w->Int(s.error_code);
    w->Key("error_message");
    w->String(s.error_message.c_str());
    w->Key("resolved_sources");
    w->StartArray();
    for (const auto& source : s.resolved_sources) {
        w->String(source.c_str());
    }
    w->EndArray();
    w->Key("source_expand_rule");
    w->String(s.source_expand_rule.c_str());
    w->Key("shared_hub_id");
    w->String(shared_hub ? shared_hub->id.c_str() : "");
    w->Key("shared_source_keys");
    w->StartArray();
    if (shared_hub) {
        for (const auto& key : shared_hub->source_keys) {
            w->String(key.c_str());
        }
    }
    w->EndArray();
    w->Key("subscriber_count");
    w->Uint(shared_hub ? static_cast<unsigned>(shared_hub->subscribers.size()) : 0);
    w->Key("subscriber_stats");
    w->StartArray();
    if (shared_hub) {
        for (const auto& sub : shared_hub->subscribers) {
            w->StartObject();
            w->Key("subscriber_id");
            w->String(sub.subscriber_id.c_str());
            w->Key("runtime_task_id");
            w->String(sub.runtime_task_id.c_str());
            w->Key("logical_node_id");
            w->String(sub.logical_node_id.c_str());
            w->Key("active");
            w->Bool(sub.active);
            w->Key("ready");
            w->Bool(sub.ready);
            w->Key("delivered_batches");
            w->Uint64(sub.delivered_batches);
            w->Key("delivered_rows");
            w->Uint64(sub.delivered_rows);
            w->Key("dropped_batches");
            w->Uint64(sub.dropped_batches);
            w->Key("dropped_rows");
            w->Uint64(sub.dropped_rows);
            w->Key("last_delivered_seq");
            w->Uint64(sub.last_delivered_seq);
            w->Key("last_dropped_seq");
            w->Uint64(sub.last_dropped_seq);
            w->Key("lag");
            w->Uint64(sub.lag);
            w->EndObject();
        }
    }
    w->EndArray();

    rapidjson::Document stats_doc;
    if (!s.op_stats_json.empty()) {
        stats_doc.Parse(s.op_stats_json.c_str());
    }
    w->Key("op_stats");
    if (stats_doc.HasParseError()) {
        w->String(s.op_stats_json.c_str());
    } else {
        rapidjson::StringBuffer stats_buf;
        rapidjson::Writer<rapidjson::StringBuffer> stats_writer(stats_buf);
        stats_doc.Accept(stats_writer);
        w->RawValue(stats_buf.GetString(), stats_buf.GetSize(),
                    stats_doc.IsArray() ? rapidjson::kArrayType : rapidjson::kObjectType);
    }
    w->EndObject();
}

void WriteGroupSnapshotJson(rapidjson::Writer<rapidjson::StringBuffer>* w,
                            const StreamGroupSnapshot& s,
                            const std::vector<SharedHubSnapshot>* share_sets,
                            const std::unordered_map<std::string, GroupNodeResolvedSourceMeta>* node_sources) {
    if (!w) return;
    uint64_t processed_rows = 0;
    uint64_t output_rows = 0;
    uint64_t dropped_batches = 0;
    uint64_t poll_errors = 0;
    for (const auto& node : s.nodes) {
        processed_rows += node.processed_rows;
        output_rows += node.output_rows;
        dropped_batches += node.dropped_batches;
        poll_errors += node.poll_errors;
    }

    w->StartObject();
    w->Key("task_id");
    w->String(s.task_id.c_str());
    w->Key("runtime_task_id");
    w->String(s.runtime_task_id.c_str());
    w->Key("runtime_kind");
    w->String("group");
    w->Key("group_mode");
    w->String(s.group_mode.c_str());
    w->Key("status");
    w->String(StreamGroupStatusName(s.status));
    w->Key("group_status");
    w->String(StreamGroupStatusName(s.status));
    w->Key("stop_requested");
    w->Bool(s.stop_requested);
    w->Key("joined");
    w->Bool(IsTerminalStreamGroupStatus(s.status));
    w->Key("node_count");
    w->Uint(s.node_count);
    w->Key("active_nodes");
    w->Uint(s.active_nodes);
    w->Key("share_set_count");
    w->Uint(static_cast<unsigned>(share_sets ? share_sets->size() : 0));
    w->Key("processed_rows");
    w->Uint64(processed_rows);
    w->Key("output_rows");
    w->Uint64(output_rows);
    w->Key("dropped_batches_shared");
    w->Uint64(dropped_batches);
    w->Key("poll_errors");
    w->Uint64(poll_errors);
    w->Key("started_ms");
    w->Int64(s.started_ms);
    w->Key("last_active_ms");
    w->Int64(s.last_active_ms);
    w->Key("finished_ms");
    w->Int64(s.finished_ms);
    w->Key("error_code");
    w->String(s.error_code.c_str());
    w->Key("error_no");
    w->Int(s.error_no);
    w->Key("error_message");
    w->String(s.error_message.c_str());

    w->Key("resolved_sources");
    w->StartArray();
    for (const auto& node : s.nodes) {
        w->StartObject();
        w->Key("node_id");
        w->String(node.node_id.c_str());
        const GroupNodeResolvedSourceMeta* source_meta = nullptr;
        if (node_sources) {
            auto it = node_sources->find(node.node_id);
            if (it != node_sources->end()) {
                source_meta = &it->second;
            }
        }
        w->Key("sources");
        w->StartArray();
        if (source_meta) {
            for (const auto& source : source_meta->sources) {
                w->String(source.c_str());
            }
        }
        w->EndArray();
        w->Key("expand_rule");
        w->String(source_meta ? source_meta->expand_rule.c_str() : "explicit");
        w->EndObject();
    }
    w->EndArray();

    w->Key("nodes");
    w->StartArray();
    for (const auto& node : s.nodes) {
        w->StartObject();
        w->Key("id");
        w->String(node.node_id.c_str());
        w->Key("runtime_task_id");
        w->String(node.runtime_task_id.c_str());
        w->Key("status");
        w->String(GroupNodeStatusName(node.status));
        w->Key("depends_on");
        w->StartArray();
        for (const auto& dep : node.depends_on) {
            w->String(dep.c_str());
        }
        w->EndArray();
        w->Key("start_condition");
        w->String(GroupStartConditionName(node.start_condition));
        w->Key("processed_rows");
        w->Uint64(node.processed_rows);
        w->Key("output_rows");
        w->Uint64(node.output_rows);
        w->Key("dropped_batches");
        w->Uint64(node.dropped_batches);
        w->Key("poll_errors");
        w->Uint64(node.poll_errors);
        w->Key("error_code");
        w->String(node.error_code.c_str());
        w->Key("error_no");
        w->Int(node.error_no);
        w->Key("last_error");
        w->String(node.error_message.c_str());
        w->Key("started_ms");
        w->Int64(node.started_ms);
        w->Key("last_active_ms");
        w->Int64(node.last_active_ms);
        w->Key("finished_ms");
        w->Int64(node.finished_ms);
        w->EndObject();
    }
    w->EndArray();

    w->Key("share_sets");
    w->StartArray();
    if (share_sets) {
        for (const auto& ss : *share_sets) {
            w->StartObject();
            w->Key("id");
            w->String(ss.id.c_str());
            w->Key("source_ref");
            w->String(ss.source_ref.c_str());
            w->Key("status");
            w->String(SharedHubStatusName(ss.status));
            w->Key("members");
            w->StartArray();
            for (const auto& member : ss.members) {
                w->String(member.c_str());
            }
            w->EndArray();
            w->Key("input_batches");
            w->Uint64(ss.input_batches);
            w->Key("delivered_batches");
            w->Uint64(ss.delivered_batches);
            w->Key("dropped_batches_shared");
            w->Uint64(ss.dropped_batches_shared);
            w->Key("drop_ratio");
            w->Double(ss.drop_ratio);
            w->Key("input_rows");
            w->Uint64(ss.input_rows);
            w->Key("delivered_rows");
            w->Uint64(ss.delivered_rows);
            w->Key("dropped_rows_shared");
            w->Uint64(ss.dropped_rows_shared);
            w->Key("last_delivered_seq");
            w->Uint64(ss.last_delivered_seq);
            w->Key("last_dropped_seq");
            w->Uint64(ss.last_dropped_seq);
            w->Key("error_code");
            w->Int(ss.error_code);
            w->Key("error_message");
            w->String(ss.error_message.c_str());
            w->EndObject();
        }
    }
    w->EndArray();

    w->EndObject();
}

}  // namespace scheduler
}  // namespace flowsql
