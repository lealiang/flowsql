/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "scheduler_plugin.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdio>
#include <chrono>
#include <common/error_code.h>
#include <common/log.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <regex>
#include <sstream>
#include <thread>
#include <type_traits>
#include <unordered_set>

#include "framework/core/channel_adapter.h"
#include "framework/core/dataframe.h"
#include "framework/core/dataframe_channel.h"
#include "framework/core/fan_in_stream_channel.h"
#include "framework/core/fan_out_stream_channel.h"
#include "framework/core/json_error_builder.h"
#include "framework/core/pipeline.h"
#include "framework/core/ring_stream_channel.h"
#include "framework/core/sql_parser.h"
#include "framework/core/sql_text_splitter.h"
#include "framework/interfaces/ichannel.h"
#include "framework/interfaces/ichannel_registry.h"
#include "framework/interfaces/idatabase_channel.h"
#include "framework/interfaces/idatabase_factory.h"
#include "framework/interfaces/idataframe_channel.h"
#include "framework/interfaces/ibuiltin_registry.h"
#include "framework/interfaces/ibridge.h"
#include "framework/interfaces/ioperator.h"
#include "framework/interfaces/ioperator_catalog.h"
#include "framework/interfaces/ioperator_registry.h"
#include "framework/interfaces/istream_channel.h"
#include "framework/interfaces/istream_factory.h"
#include "framework/interfaces/istream_manager.h"
#include "scheduler_json_codec.h"
#include "scheduler_internal_utils.h"

namespace flowsql {
namespace scheduler {

static std::shared_ptr<IChannel> MakeNonOwningChannelHolder(IChannel* ch) {
    if (!ch) return nullptr;
    return std::shared_ptr<IChannel>(ch, [](IChannel*) {});
}

static std::shared_ptr<IStreamChannel> MakeStreamOwner(IStreamChannel* stream_ch,
                                                       const std::shared_ptr<IChannel>& owner) {
    if (!stream_ch) return nullptr;
    if (owner) {
        auto stream_owner = std::dynamic_pointer_cast<IStreamChannel>(owner);
        if (stream_owner) return stream_owner;
    }
    return std::shared_ptr<IStreamChannel>(stream_ch, [](IStreamChannel*) {});
}

static const char* DataTypeName(DataType t) {
    switch (t) {
        case DataType::INT32: return "INT32";
        case DataType::INT64: return "INT64";
        case DataType::UINT32: return "UINT32";
        case DataType::UINT64: return "UINT64";
        case DataType::FLOAT: return "FLOAT";
        case DataType::DOUBLE: return "DOUBLE";
        case DataType::STRING: return "STRING";
        case DataType::BYTES: return "BYTES";
        case DataType::TIMESTAMP: return "TIMESTAMP";
        case DataType::BOOLEAN: return "BOOLEAN";
        default: return "UNKNOWN";
    }
}

void SchedulerPlugin::EnumRoutes(std::function<void(const RouteItem&)> cb) {
    // 任务执行
    cb({"POST", "/scheduler/batch/execute",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleExecute(u, req, rsp);
        }});
    cb({"POST", "/scheduler/batch/submit",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleBatchSubmit(u, req, rsp);
        }});
    cb({"POST", "/scheduler/batch/status",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleBatchStatus(u, req, rsp);
        }});
    cb({"POST", "/scheduler/batch/stop",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleBatchStop(u, req, rsp);
        }});
    cb({"POST", "/scheduler/sql/classify",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleSqlClassify(u, req, rsp);
        }});
    cb({"POST", "/scheduler/stream/execute",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleStreamExecute(u, req, rsp);
        }});
    cb({"POST", "/scheduler/stream/stop",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleStreamStop(u, req, rsp);
        }});
    cb({"POST", "/scheduler/stream/status",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleStreamStatus(u, req, rsp);
        }});
    cb({"POST", "/scheduler/stream/list",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleStreamList(u, req, rsp);
        }});
    cb({"POST", "/scheduler/runtime/graph/query",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleRuntimeGraphQuery(u, req, rsp);
        }});
    // 流式通道查询（管理面最小字段）
    cb({"POST", "/channels/stream/query",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleQueryStreamChannels(u, req, rsp);
        }});
    cb({"POST", "/channels/stream/definitions/query",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleQueryStreamChannelDefinitions(u, req, rsp);
        }});
    cb({"POST", "/channels/stream/add",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleAddStreamChannel(u, req, rsp);
        }});
    cb({"POST", "/channels/stream/modify",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleModifyStreamChannel(u, req, rsp);
        }});
    cb({"POST", "/channels/stream/remove",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleRemoveStreamChannel(u, req, rsp);
        }});
    // 内存通道查询
    cb({"POST", "/channels/dataframe/query",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleGetChannels(u, req, rsp);
        }});
    // 内存通道数据预览
    cb({"POST", "/channels/dataframe/preview",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandlePreviewDataframe(u, req, rsp);
        }});
    // Python 算子刷新
    cb({"POST", "/operators/python/refresh",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleRefreshOperators(u, req, rsp);
        }});
}

int32_t SchedulerPlugin::HandleSqlClassify(const std::string&, const std::string& req_body, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req_body.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("sql") || !doc["sql"].IsString()) {
        rsp = BuildErrorJson("invalid request, expected {\"sql\":\"...\"}");
        return error::BAD_REQUEST;
    }

    std::string task_kind;
    std::string err_rsp;
    const int32_t rc = ClassifySqlTaskKind(doc["sql"].GetString(), &task_kind, &err_rsp);
    if (rc != error::OK) {
        rsp = err_rsp.empty() ? BuildErrorJson("sql classify failed") : err_rsp;
        return rc;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("task_kind");
    w.String(task_kind.c_str());
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t SchedulerPlugin::HandleBatchSubmit(const std::string&, const std::string& req_body, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req_body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        rsp = BuildErrorJson("invalid request body");
        return error::BAD_REQUEST;
    }

    std::string runtime_task_id;
    if (doc.HasMember("runtime_task_id")) {
        if (!doc["runtime_task_id"].IsString()) {
            rsp = BuildErrorJson("runtime_task_id must be string");
            return error::BAD_REQUEST;
        }
        runtime_task_id = doc["runtime_task_id"].GetString();
    }
    if (runtime_task_id.empty()) {
        runtime_task_id = "b_" + NextStreamTaskId();
    }

    int timeout_s = 0;
    if (doc.HasMember("timeout_s")) {
        if (!doc["timeout_s"].IsInt()) {
            rsp = BuildErrorJson("timeout_s must be integer");
            return error::BAD_REQUEST;
        }
        timeout_s = doc["timeout_s"].GetInt();
        if (timeout_s < 0) {
            rsp = BuildErrorJson("timeout_s must be >= 0");
            return error::BAD_REQUEST;
        }
    }

    std::vector<std::string> sqls;
    if (doc.HasMember("sqls")) {
        if (!doc["sqls"].IsArray() || doc["sqls"].Empty()) {
            rsp = BuildErrorJson("sqls must be non-empty string array");
            return error::BAD_REQUEST;
        }
        for (const auto& it : doc["sqls"].GetArray()) {
            if (!it.IsString()) {
                rsp = BuildErrorJson("sqls must be non-empty string array");
                return error::BAD_REQUEST;
            }
            std::string sql = it.GetString();
            if (sql.empty()) {
                rsp = BuildErrorJson("sqls must not contain empty SQL");
                return error::BAD_REQUEST;
            }
            sqls.push_back(sql);
        }
    } else if (doc.HasMember("sql_text") && doc["sql_text"].IsString()) {
        SqlTextSplitError split_err;
        if (SplitSqlText(doc["sql_text"].GetString(), &sqls, &split_err) != 0) {
            std::string err = "invalid sql_text";
            if (!split_err.message.empty()) err += ": " + split_err.message;
            rsp = BuildErrorJson(err);
            return error::BAD_REQUEST;
        }
    } else {
        rsp = BuildErrorJson("request must contain sqls or sql_text");
        return error::BAD_REQUEST;
    }

    for (size_t i = 0; i < sqls.size(); ++i) {
        std::string task_kind;
        std::string classify_err_rsp;
        const int32_t classify_rc = ClassifySqlTaskKind(sqls[i], &task_kind, &classify_err_rsp);
        if (classify_rc != error::OK) {
            rsp = classify_err_rsp.empty() ? BuildErrorJson("sql classify failed") : classify_err_rsp;
            return classify_rc;
        }
        if (task_kind != "batch") {
            rsp = BuildErrorJson("batch submit only accepts batch SQL");
            return error::BAD_REQUEST;
        }
    }

    std::string submit_err;
    const int submit_rc = batch_runtime_.Submit(runtime_task_id, std::move(sqls), timeout_s, &submit_err);
    if (submit_rc != 0) {
        if (submit_rc == EEXIST) {
            rsp = BuildErrorJson("runtime_task_id already exists: " + runtime_task_id);
            return error::CONFLICT;
        }
        rsp = BuildErrorJson("batch submit failed: " + submit_err);
        return error::INTERNAL_ERROR;
    }
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("status");
    w.String("submitted");
    w.Key("runtime_task_id");
    w.String(runtime_task_id.c_str());
    w.Key("runtime_kind");
    w.String("batch");
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t SchedulerPlugin::HandleBatchStatus(const std::string&, const std::string& req_body, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req_body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        rsp = BuildErrorJson("invalid request body");
        return error::BAD_REQUEST;
    }
    std::string runtime_task_id;
    if (doc.HasMember("runtime_task_id") && doc["runtime_task_id"].IsString()) {
        runtime_task_id = doc["runtime_task_id"].GetString();
    } else if (doc.HasMember("task_id") && doc["task_id"].IsString()) {
        runtime_task_id = doc["task_id"].GetString();
    }
    if (runtime_task_id.empty()) {
        rsp = BuildErrorJson("runtime_task_id is required");
        return error::BAD_REQUEST;
    }

    BatchRuntimeSnapshot snapshot;
    const int query_rc = batch_runtime_.Query(runtime_task_id, &snapshot);
    if (query_rc != 0) {
        rsp = BuildErrorJson("batch runtime task not found: " + runtime_task_id);
        return error::NOT_FOUND;
    }
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    batch_runtime_.SweepFinished(now_ms, stream_runtime_retention_s_, stream_runtime_max_count_);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("runtime_task_id");
    w.String(snapshot.runtime_task_id.c_str());
    w.Key("runtime_kind");
    w.String("batch");
    w.Key("status");
    w.String(BatchRuntimeStatusName(snapshot.status));
    w.Key("error_code");
    w.String(snapshot.error_code.c_str());
    w.Key("error_message");
    w.String(snapshot.error_message.c_str());
    w.Key("error_stage");
    w.String(snapshot.error_stage.c_str());
    w.Key("current_sql_index");
    w.Int(snapshot.current_sql_index);
    w.Key("sql_count");
    w.Int(snapshot.sql_count);
    w.Key("timeout_s");
    w.Int(snapshot.timeout_s);
    w.Key("result_row_count");
    w.Int64(snapshot.result_row_count);
    w.Key("result_col_count");
    w.Int64(snapshot.result_col_count);
    w.Key("result_target");
    w.String(snapshot.result_target.c_str());
    w.Key("created_ms");
    w.Int64(snapshot.created_ms);
    w.Key("started_ms");
    w.Int64(snapshot.started_ms);
    w.Key("last_active_ms");
    w.Int64(snapshot.last_active_ms);
    w.Key("finished_ms");
    w.Int64(snapshot.finished_ms);
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t SchedulerPlugin::HandleBatchStop(const std::string&, const std::string& req_body, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req_body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        rsp = BuildErrorJson("invalid request body");
        return error::BAD_REQUEST;
    }
    std::string runtime_task_id;
    if (doc.HasMember("runtime_task_id") && doc["runtime_task_id"].IsString()) {
        runtime_task_id = doc["runtime_task_id"].GetString();
    } else if (doc.HasMember("task_id") && doc["task_id"].IsString()) {
        runtime_task_id = doc["task_id"].GetString();
    }
    if (runtime_task_id.empty()) {
        rsp = BuildErrorJson("runtime_task_id is required");
        return error::BAD_REQUEST;
    }

    std::string stop_err;
    const int stop_rc = batch_runtime_.RequestStop(runtime_task_id, &stop_err);
    if (stop_rc != 0) {
        rsp = BuildErrorJson("batch stop failed: " + stop_err);
        return stop_rc == ENOENT ? error::NOT_FOUND : error::BAD_REQUEST;
    }
    return HandleBatchStatus("", std::string("{\"runtime_task_id\":\"") + runtime_task_id + "\"}", rsp);
}

int32_t SchedulerPlugin::HandleStreamExecuteSingle(const rapidjson::Document& doc, std::string& rsp) {
    if (doc.HasMember("group_mode") ||
        doc.HasMember("dag") ||
        doc.HasMember("sql") ||
        doc.HasMember("sqls") ||
        doc.HasMember("share_set_ready_timeout_s")) {
        rsp = BuildExecutionErrorJson(
            "single execution accepts only sql_text and timeout_s",
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kRequest);
        return error::BAD_REQUEST;
    }
    if (!doc.HasMember("sql_text") || !doc["sql_text"].IsString()) {
        rsp = BuildExecutionErrorJson(
            "invalid request, expected {\"sql_text\":\"...\"}",
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kRequest);
        return error::BAD_REQUEST;
    }
    std::vector<std::string> sqls;
    SqlTextSplitError split_err;
    if (SplitSqlText(doc["sql_text"].GetString(), &sqls, &split_err) != 0) {
        std::string err = "invalid sql_text";
        if (!split_err.message.empty()) {
            err += ": " + split_err.message;
        }
        rsp = BuildExecutionErrorWithSqlIndexJson(
            err,
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kRequest,
            split_err.statement_index);
        return error::BAD_REQUEST;
    }
    if (sqls.size() != 1) {
        rsp = BuildExecutionErrorJson(
            "single execution requires exactly one SQL statement",
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kRequest);
        return error::BAD_REQUEST;
    }

    SqlParser parser;
    SqlStatement stmt = parser.Parse(sqls.front());
    if (!stmt.error.empty()) {
        rsp = BuildExecutionErrorJson(
            stmt.error,
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kParse);
        return error::BAD_REQUEST;
    }
    if (stmt.sources.empty() && !stmt.source.empty()) {
        stmt.sources.push_back(stmt.source);
    }
    if (stmt.sources.empty()) {
        rsp = BuildExecutionErrorJson(
            "source channel not found",
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kParse);
        return error::BAD_REQUEST;
    }
    return ExecuteStreamTask(stmt, rsp);
}

int32_t SchedulerPlugin::HandleStreamExecute(const std::string&, const std::string& req_body, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req_body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        rsp = BuildExecutionErrorJson(
            "invalid request body",
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kRequest);
        return error::BAD_REQUEST;
    }
    if (doc.HasMember("task_id")) {
        rsp = BuildExecutionErrorJson(
            "external task_id is not allowed",
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kRequest);
        return error::BAD_REQUEST;
    }

    std::string execution_kind = "single";
    if (doc.HasMember("execution_kind")) {
        if (!doc["execution_kind"].IsString()) {
            rsp = BuildExecutionErrorJson(
                "execution_kind must be string",
                ErrorCodeId::kStreamGroupSqlTextInvalid,
                ErrorStageId::kRequest);
            return error::BAD_REQUEST;
        }
        execution_kind = ToLowerAscii(doc["execution_kind"].GetString());
    }

    if (execution_kind == "single") {
        return HandleStreamExecuteSingle(doc, rsp);
    }
    if (execution_kind == "group") {
        return HandleStreamExecuteGroup(doc, rsp);
    }

    rsp = BuildExecutionErrorJson(
        "unsupported execution_kind: " + execution_kind,
        ErrorCodeId::kStreamGroupSqlTextInvalid,
        ErrorStageId::kRequest);
    return error::BAD_REQUEST;
}

int32_t SchedulerPlugin::HandleStreamStop(const std::string&, const std::string& req, std::string& rsp) {
    SweepFinishedTaskLeases();
    SweepRuntimeRetainedObjects();
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("task_id") || !doc["task_id"].IsString()) {
        rsp = BuildErrorJson("invalid request, expected {\"task_id\":\"...\"}");
        return error::BAD_REQUEST;
    }
    const std::string task_id = doc["task_id"].GetString();
    {
        std::lock_guard<std::mutex> lock(stream_group_nodes_mu_);
        if (stream_group_node_owners_.find(task_id) != stream_group_node_owners_.end()) {
            rsp = BuildErrorJson("group node runtime_task_id is internal; use group task_id");
            return error::BAD_REQUEST;
        }
    }

    std::shared_ptr<StreamTaskGroup> group;
    {
        std::lock_guard<std::mutex> lock(stream_task_groups_mu_);
        auto it = stream_task_groups_.find(task_id);
        if (it != stream_task_groups_.end()) {
            group = it->second;
        }
    }
    if (group) {
        group->RequestStop();
        group->Join();
        StreamGroupSnapshot snapshot = group->Snapshot();
        TouchRuntimeAccess(task_id);
        if (IsTerminalStreamGroupStatus(snapshot.status)) {
            MarkRuntimeTerminal(task_id, "group");
        }
        const auto node_sources = QueryGroupNodeResolvedSources(task_id);
        const auto share_sets = QueryGroupShareSetSnapshots(task_id);
        CleanupGroupRuntimeResources(task_id, &snapshot);
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        WriteGroupSnapshotJson(&w, snapshot, &share_sets,
                               node_sources.empty() ? nullptr : &node_sources);
        rsp = buf.GetString();
        SweepRuntimeRetainedObjects();
        return error::OK;
    }

    std::shared_ptr<StreamTask> task;
    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        auto it = stream_tasks_.find(task_id);
        if (it == stream_tasks_.end()) {
            rsp = BuildErrorJson("stream task not found: " + task_id);
            return error::NOT_FOUND;
        }
        task = it->second;
    }

    task->RequestStop();
    task->Join();
    ReleaseStreamTaskLeases(task_id);
    TouchRuntimeAccess(task_id);
    MarkRuntimeTerminal(task_id, "single");

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    WriteTaskSnapshotJson(&w, task->Snapshot(), nullptr);
    rsp = buf.GetString();
    SweepRuntimeRetainedObjects();
    return error::OK;
}

int32_t SchedulerPlugin::HandleStreamStatus(const std::string&, const std::string& req, std::string& rsp) {
    SweepFinishedTaskLeases();
    SweepRuntimeRetainedObjects();
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("task_id") || !doc["task_id"].IsString()) {
        rsp = BuildErrorJson("invalid request, expected {\"task_id\":\"...\"}");
        return error::BAD_REQUEST;
    }
    const std::string task_id = doc["task_id"].GetString();
    {
        std::lock_guard<std::mutex> lock(stream_group_nodes_mu_);
        if (stream_group_node_owners_.find(task_id) != stream_group_node_owners_.end()) {
            rsp = BuildErrorJson("group node runtime_task_id is internal; use group task_id");
            return error::BAD_REQUEST;
        }
    }

    std::shared_ptr<StreamTaskGroup> group;
    {
        std::lock_guard<std::mutex> lock(stream_task_groups_mu_);
        auto it = stream_task_groups_.find(task_id);
        if (it != stream_task_groups_.end()) {
            group = it->second;
        }
    }
    if (group) {
        StreamGroupSnapshot snapshot = group->Snapshot();
        TouchRuntimeAccess(task_id);
        if (IsTerminalStreamGroupStatus(snapshot.status)) {
            MarkRuntimeTerminal(task_id, "group");
        }
        const auto node_sources = QueryGroupNodeResolvedSources(task_id);
        const auto share_sets = QueryGroupShareSetSnapshots(task_id);
        if (IsTerminalStreamGroupStatus(snapshot.status)) {
            CleanupGroupRuntimeResources(task_id, &snapshot);
        }
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        WriteGroupSnapshotJson(&w, snapshot, &share_sets,
                               node_sources.empty() ? nullptr : &node_sources);
        rsp = buf.GetString();
        SweepRuntimeRetainedObjects();
        return error::OK;
    }

    std::shared_ptr<StreamTask> task;
    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        auto it = stream_tasks_.find(task_id);
        if (it == stream_tasks_.end()) {
            rsp = BuildErrorJson("stream task not found: " + task_id);
            return error::NOT_FOUND;
        }
        task = it->second;
    }

    TaskSnapshot snapshot = task->Snapshot();
    TouchRuntimeAccess(task_id);
    if (IsTerminalStreamTaskStatus(snapshot.status)) {
        MarkRuntimeTerminal(task_id, "single");
        ReleaseStreamTaskLeases(task_id);
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    SharedHubSnapshot shared_hub_snapshot;
    SharedHubSnapshot* shared_hub_ptr = nullptr;
    if (QueryRuntimeSharedHubSnapshot(task_id, &shared_hub_snapshot) == 0) {
        shared_hub_ptr = &shared_hub_snapshot;
    }
    WriteTaskSnapshotJson(&w, snapshot, shared_hub_ptr);
    rsp = buf.GetString();
    SweepRuntimeRetainedObjects();
    return error::OK;
}

int32_t SchedulerPlugin::HandleStreamList(const std::string&, const std::string&, std::string& rsp) {
    SweepFinishedTaskLeases();
    SweepRuntimeRetainedObjects();
    std::vector<TaskSnapshot> snapshots;
    std::vector<StreamGroupSnapshot> group_snapshots;
    std::vector<std::string> terminal_tasks;
    std::unordered_set<std::string> internal_node_ids;
    {
        std::lock_guard<std::mutex> lock(stream_group_nodes_mu_);
        internal_node_ids.reserve(stream_group_node_owners_.size());
        for (const auto& kv : stream_group_node_owners_) {
            internal_node_ids.insert(kv.first);
        }
    }
    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        snapshots.reserve(stream_tasks_.size());
        for (const auto& kv : stream_tasks_) {
            if (internal_node_ids.count(kv.first) > 0) continue;
            if (!kv.second) continue;
            TaskSnapshot s = kv.second->Snapshot();
            TouchRuntimeAccess(kv.first);
            if (IsTerminalStreamTaskStatus(s.status)) {
                terminal_tasks.push_back(kv.first);
                MarkRuntimeTerminal(kv.first, "single");
            }
            snapshots.push_back(std::move(s));
        }
    }
    {
        std::lock_guard<std::mutex> lock(stream_task_groups_mu_);
        group_snapshots.reserve(stream_task_groups_.size());
        for (const auto& kv : stream_task_groups_) {
            if (!kv.second) continue;
            StreamGroupSnapshot s = kv.second->Snapshot();
            TouchRuntimeAccess(kv.first);
            if (IsTerminalStreamGroupStatus(s.status)) {
                MarkRuntimeTerminal(kv.first, "group");
                for (const auto& node : s.nodes) {
                    terminal_tasks.push_back(node.runtime_task_id);
                }
            }
            group_snapshots.push_back(std::move(s));
        }
    }
    for (const auto& task_id : terminal_tasks) {
        ReleaseStreamTaskLeases(task_id);
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("tasks");
    w.StartArray();
    for (const auto& s : snapshots) {
        SharedHubSnapshot shared_hub_snapshot;
        SharedHubSnapshot* shared_hub_ptr = nullptr;
        if (QueryRuntimeSharedHubSnapshot(s.task_id, &shared_hub_snapshot) == 0) {
            shared_hub_ptr = &shared_hub_snapshot;
        }
        WriteTaskSnapshotJson(&w, s, shared_hub_ptr);
    }
    for (const auto& s : group_snapshots) {
        const auto node_sources = QueryGroupNodeResolvedSources(s.task_id);
        const auto share_sets = QueryGroupShareSetSnapshots(s.task_id);
        if (IsTerminalStreamGroupStatus(s.status)) {
            CleanupGroupRuntimeResources(s.task_id, &s);
        }
        WriteGroupSnapshotJson(&w, s, &share_sets, node_sources.empty() ? nullptr : &node_sources);
    }
    w.EndArray();
    w.EndObject();
    rsp = buf.GetString();
    SweepRuntimeRetainedObjects();
    return error::OK;
}

// 逻辑链：
// 1) 解析并校验 SQL，请求路由先区分 stream/batch 源类型；
// 2) batch 路径解析算子链并绑定 source/sink；
// 3) 执行纯传输或算子链执行，统一抽取错误阶段并返回标准错误结构；
// 4) INTO dataframe.<name> 场景写回注册中心并组织最终响应。
int32_t SchedulerPlugin::HandleExecute(const std::string&, const std::string& req_body, std::string& rsp) {
    auto* ch_registry = querier_ ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY)) : nullptr;
    rapidjson::Document doc;
    doc.Parse(req_body.c_str());
    if (doc.HasParseError() || !doc.HasMember("sql") || !doc["sql"].IsString()) {
        rsp = BuildErrorJson("invalid request, expected {\"sql\":\"...\"}");
        return error::BAD_REQUEST;
    }
    std::string sql_text = doc["sql"].GetString();

    static constexpr size_t kMaxSqlLength = 64 * 1024;
    if (sql_text.size() > kMaxSqlLength) {
        rsp = BuildErrorJson("SQL too long (max 64KB)");
        return error::BAD_REQUEST;
    }

    SqlParser parser;
    auto stmt = parser.Parse(sql_text);
    if (!stmt.error.empty()) {
        rsp = BuildErrorJson(stmt.error);
        return error::BAD_REQUEST;
    }

    if (stmt.sources.empty() && !stmt.source.empty()) {
        stmt.sources.push_back(stmt.source);
    }
    if (stmt.sources.empty()) {
        rsp = BuildErrorJson("source channel not found");
        return error::BAD_REQUEST;
    }

    SourceResolveResult source_resolved;
    std::string source_err_rsp;
    const int32_t source_rc = ResolveSourceBindings(stmt, &source_resolved, &source_err_rsp);
    if (source_rc != error::OK) {
        rsp = source_err_rsp.empty() ? BuildErrorJson("source resolve failed") : source_err_rsp;
        return source_rc;
    }
    if (source_resolved.has_stream_source && source_resolved.has_non_stream_source) {
        rsp = BuildErrorJson("mixed stream and non-stream sources are not supported");
        return error::BAD_REQUEST;
    }
    if (source_resolved.has_stream_source) {
        return ExecuteStreamTask(stmt, rsp);
    }

    std::vector<IChannel*> input_channels = source_resolved.channels;

    std::vector<std::shared_ptr<IOperator>> op_holders;
    std::vector<IOperator*> op_chain;
    std::vector<OperatorRef> parsed_ops = stmt.operators;
    if (parsed_ops.empty() && !stmt.op_category.empty() && !stmt.op_name.empty()) {
        parsed_ops.push_back({stmt.op_category, stmt.op_name});
    }
    if (!parsed_ops.empty()) {
        auto* catalog = querier_ ? static_cast<IOperatorCatalog*>(querier_->First(IID_OPERATOR_CATALOG)) : nullptr;
        if (!catalog) {
            rsp = BuildErrorJson("operator catalog unavailable");
            return error::UNAVAILABLE;
        }
        for (const auto& op_ref : parsed_ops) {
            OperatorStatus status = catalog->QueryStatus(op_ref.category, op_ref.name);
            if (status == OperatorStatus::kNotFound) {
                rsp = BuildErrorJson("operator not found: " + op_ref.category + "." + op_ref.name);
                return error::NOT_FOUND;
            }
            if (status == OperatorStatus::kDeactivated) {
                rsp = BuildErrorJson("operator is deactivated: " + op_ref.category + "." + op_ref.name);
                return error::CONFLICT;
            }
            auto holder = FindOperator(op_ref.category, op_ref.name);
            if (!holder) {
                rsp = BuildErrorJson("operator not found: " + op_ref.category + "." + op_ref.name);
                return error::NOT_FOUND;
            }
            op_chain.push_back(holder.get());
            op_holders.push_back(std::move(holder));
        }
    }

    try {
        if (!op_chain.empty()) {
            for (size_t i = 0; i < op_chain.size(); ++i) {
                const auto& params = (i < stmt.operator_with_params.size())
                    ? stmt.operator_with_params[i]
                    : (i == 0 ? stmt.with_params : std::unordered_map<std::string, std::string>{});
                for (const auto& kv : params) {
                    op_chain[i]->Configure(kv.first.c_str(), kv.second.c_str());
                }
            }
        }

        std::shared_ptr<DataFrameChannel> temp_sink;
        std::shared_ptr<IDataFrameChannel> named_df_sink;
        std::shared_ptr<IChannel> named_sink_holder;
        IChannel* sink = nullptr;

        if (!stmt.dest.empty()) {
            if (!IsQualifiedDestination(stmt.dest)) {
                rsp = BuildErrorJson("invalid INTO destination: " + stmt.dest +
                                    ", expected dataframe.<name> or <type>.<name>[.<table>]");
                return error::BAD_REQUEST;
            }
            if (IsDataframeRefName(stmt.dest)) {
                if (!ch_registry) {
                    rsp = BuildErrorJson("channel registry unavailable");
                    return error::INTERNAL_ERROR;
                }
                std::string df_name = DataframeNamePart(stmt.dest);
                auto ch = std::make_shared<DataFrameChannel>("dataframe", df_name);
                ch->Open();
                named_df_sink = ch;
                sink = ch.get();
            } else {
                sink = FindChannel(stmt.dest, &named_sink_holder);
                if (!sink) {
                    rsp = BuildErrorJson("destination channel not found: " + stmt.dest);
                    return error::NOT_FOUND;
                }
            }
        } else {
            temp_sink = std::make_shared<DataFrameChannel>("_temp", "sink");
            temp_sink->Open();
            sink = temp_sink.get();
        }

        int rc = 0;
        int64_t affected_rows = 0;
        std::string exec_error;
        std::string sink_type(sink->Type());

        if (input_channels.size() > 1 && op_chain.empty()) {
            rsp = BuildErrorJson("multi-source FROM requires USING operator");
            return error::BAD_REQUEST;
        }
        if (input_channels.size() > 1) {
            for (const auto& source_name : stmt.sources) {
                if (!IsDataframeRefName(source_name)) {
                    rsp = BuildErrorJson("multi-source FROM only supports dataframe.* in Sprint 10");
                    return error::BAD_REQUEST;
                }
            }
            if (!stmt.where_clause.empty()) {
                rsp = BuildErrorJson("multi-source FROM does not support WHERE in Sprint 10");
                return error::BAD_REQUEST;
            }
        }

        if (op_chain.empty()) {
            if (input_channels.size() != 1) {
                rsp = BuildErrorJson("invalid source count");
                return error::BAD_REQUEST;
            }
            IChannel* source = input_channels[0];
            std::string source_type(source->Type());
            rc = ExecuteTransfer(source, sink, source_type, sink_type, stmt, &affected_rows, &exec_error);
        } else {
            rc = ExecuteWithOperatorChain(Span<IChannel*>(input_channels), sink, op_chain, sink_type, stmt,
                                          &affected_rows, &exec_error);
        }

        if (rc != 0) {
            std::string err = exec_error;
            if (err.empty() && !op_chain.empty()) err = op_chain.back()->LastError();
            if (err.empty()) err = "execution failed";
            std::string stage = ExtractStageFromExecutionError(err);
            if (stage.empty()) stage = "execute";
            rsp = BuildExecutionErrorJson(err, ErrorCodeId::kOpExecFail, stage);
            return error::INTERNAL_ERROR;
        }

        // INTO dataframe.<name>：覆盖语义（已存在则先注销，再注册新结果）
        if (!stmt.dest.empty() && IsDataframeRefName(stmt.dest) && named_df_sink) {
            std::string df_name = DataframeNamePart(stmt.dest);
            if (ch_registry->Get(df_name.c_str())) {
                (void)ch_registry->Unregister(df_name.c_str());
            }
            if (ch_registry->Register(df_name.c_str(), std::static_pointer_cast<IChannel>(named_df_sink)) != 0) {
                rsp = BuildErrorJson("failed to register dataframe channel: " + df_name);
                return error::INTERNAL_ERROR;
            }
            auto registered = ch_registry->Get(df_name.c_str());
            if (!registered) {
                rsp = BuildErrorJson("failed to fetch registered dataframe channel: " + df_name);
                return error::INTERNAL_ERROR;
            }
            auto* registered_df = dynamic_cast<IDataFrameChannel*>(registered.get());
            if (!registered_df) {
                rsp = BuildErrorJson("registered channel is not dataframe: " + df_name);
                return error::INTERNAL_ERROR;
            }
            sink = registered_df;
            sink_type = sink->Type();
        }

        auto* df_sink = dynamic_cast<IDataFrameChannel*>(sink);
        DataFrame result;
        std::string result_json = "[]";
        int64_t row_count = 0;
        if (df_sink && df_sink->Read(&result) == 0 && result.RowCount() > 0) {
            result_json = result.ToJson();
            row_count = result.RowCount();
        } else if (sink_type == ChannelType::kDatabase) {
            row_count = affected_rows;
        }

        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("status"); w.String("completed");
        w.Key("rows"); w.Int64(row_count);
        w.Key("result_row_count"); w.Int64(row_count);
        w.Key("result_target"); w.String(stmt.dest.c_str());
        w.Key("data"); w.RawValue(result_json.c_str(), result_json.size(), rapidjson::kArrayType);
        w.EndObject();
        rsp = buf.GetString();
        return error::OK;

    } catch (const std::exception& e) {
        std::string err = std::string("internal error: ") + e.what();
        LOG_ERROR("SchedulerPlugin::HandleExecute: exception: %s", err.c_str());
        rsp = BuildErrorJson(err);
        return error::INTERNAL_ERROR;
    } catch (...) {
        LOG_ERROR("SchedulerPlugin::HandleExecute: unknown exception");
        rsp = BuildErrorJson("internal error: unknown exception");
        return error::INTERNAL_ERROR;
    }
}

// --- HandleGetChannels ---
int32_t SchedulerPlugin::HandleGetChannels(const std::string&, const std::string&, std::string& rsp) {
    auto* ch_registry = querier_ ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY)) : nullptr;
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();

    // 内部通道表
    auto managed_snapshot = SnapshotManagedChannels();
    for (auto& [key, ch_ptr] : managed_snapshot) {
        auto* ch = ch_ptr.get();
        w.StartObject();
        w.Key("category"); w.String(ch->Category());
        w.Key("name"); w.String(ch->Name());
        w.Key("type"); w.String(ch->Type());
        w.Key("schema"); w.String(ch->Schema());
        w.EndObject();
    }

    // 具名 DataFrame 通道（CatalogPlugin 注册中心）
    if (ch_registry) {
        ch_registry->List([&w](const char* name, std::shared_ptr<IChannel> ch) {
            if (!name || !ch) return;
            w.StartObject();
            w.Key("category"); w.String(ch->Category());
            w.Key("name"); w.String(name);
            w.Key("type"); w.String(ch->Type());
            w.Key("schema"); w.String(ch->Schema());
            w.EndObject();
        });
    }

    // 静态注册的通道（通过 IQuerier）
    if (querier_) {
        querier_->Traverse(IID_CHANNEL, [&w](void* p) -> int {
            auto* ch = static_cast<IChannel*>(p);
            w.StartObject();
            w.Key("category"); w.String(ch->Category());
            w.Key("name"); w.String(ch->Name());
            w.Key("type"); w.String(ch->Type());
            w.Key("schema"); w.String(ch->Schema());
            w.EndObject();
            return 0;
        });

        // 数据库通道（通过 IDatabaseFactory）
        auto* factory = static_cast<IDatabaseFactory*>(querier_->First(IID_DATABASE_FACTORY));
        if (factory) {
            factory->List([&w](const char* type, const char* name, const char* config_json) {
                w.StartObject();
                w.Key("category"); w.String(type);
                w.Key("name"); w.String(name);
                w.Key("type"); w.String(ChannelType::kDatabase);
                // 从 config_json 提取 database 字段作为 schema 展示
                std::string db_label;
                if (config_json) {
                    rapidjson::Document cfg;
                    cfg.Parse(config_json);
                    if (!cfg.HasParseError() && cfg.IsObject()) {
                        if (cfg.HasMember("database") && cfg["database"].IsString()) {
                            db_label = cfg["database"].GetString();
                        } else if (cfg.HasMember("path") && cfg["path"].IsString()) {
                            db_label = cfg["path"].GetString();
                        }
                    }
                }
                w.Key("schema"); w.String(db_label.c_str());
                w.EndObject();
            });
        }

        // 流式通道（通过 IStreamFactory）
        auto* stream_factory = static_cast<IStreamFactory*>(querier_->First(IID_STREAM_FACTORY));
        if (stream_factory) {
            stream_factory->List([&w](const char* type, const char* name, IStreamChannel* stream_ch) {
                if (!stream_ch || !type || !name) return;
                w.StartObject();
                w.Key("category"); w.String(type);
                w.Key("name"); w.String(name);
                w.Key("type"); w.String(stream_ch->Type());
                w.Key("schema"); w.String(stream_ch->Schema());
                w.EndObject();
            });
        }
    }

    w.EndArray();
    rsp = buf.GetString();
    return error::OK;
}

// --- HandleQueryStreamChannelDefinitions ---
int32_t SchedulerPlugin::HandleQueryStreamChannelDefinitions(const std::string&,
                                                             const std::string&,
                                                             std::string& rsp) {
    auto* builtin_registry = querier_ ? static_cast<IBuiltinRegistry*>(querier_->First(IID_BUILTIN_REGISTRY)) : nullptr;
    if (!builtin_registry) {
        rsp = BuildErrorJson("builtin registry unavailable");
        return error::UNAVAILABLE;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("definitions");
    w.StartArray();

    builtin_registry->ListStreamChannelTypes([&w](const StreamChannelTypeDescriptor& def) {
        w.StartObject();
        w.Key("channel_type");
        w.String(def.type.c_str());
        w.Key("display_name");
        w.String(def.display_name.c_str());

        w.Key("allowed_roles");
        w.StartArray();
        for (const auto& role : def.allowed_roles) {
            w.String(role.c_str());
        }
        w.EndArray();

        w.Key("option_schema");
        w.StartArray();
        for (const auto& field : def.option_schema) {
            w.StartObject();
            w.Key("key");
            w.String(field.key.c_str());
            w.Key("type");
            w.String(field.type.c_str());
            w.Key("required");
            w.Bool(field.required);
            w.Key("default_value");
            w.String(field.default_value.c_str());
            w.Key("enum_values");
            w.StartArray();
            for (const auto& value : field.enum_values) {
                w.String(value.c_str());
            }
            w.EndArray();
            w.Key("min_value");
            w.Int64(field.min_value);
            w.Key("max_value");
            w.Int64(field.max_value);
            w.Key("has_range");
            w.Bool(field.has_range);
            w.Key("power_of_two");
            w.Bool(field.power_of_two);
            w.Key("desc");
            w.String(field.desc.c_str());
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
    });

    w.EndArray();
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

// --- HandleQueryStreamChannels ---
int32_t SchedulerPlugin::HandleQueryStreamChannels(const std::string&,
                                                   const std::string&,
                                                   std::string& rsp) {
    SweepFinishedTaskLeases();
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("channels");
    w.StartArray();

    auto* stream_manager = querier_ ? static_cast<IStreamManager*>(querier_->First(IID_STREAM_MANAGER)) : nullptr;
    auto* stream_factory = querier_ ? static_cast<IStreamFactory*>(querier_->First(IID_STREAM_FACTORY)) : nullptr;
    if (stream_manager) {
        stream_manager->QueryChannels([this, stream_factory, &w](const std::string& type,
                                                                 const std::string& name,
                                                                 const std::string& option,
                                                                 const std::string& status) {
            const std::string key = MakeStreamChannelKey(type, name);
            uint32_t in_use_count = 0;
            {
                std::lock_guard<std::mutex> lock(stream_channel_refs_mu_);
                auto it = stream_channel_ref_counts_.find(key);
                if (it != stream_channel_ref_counts_.end()) in_use_count = it->second;
            }
            IStreamChannel* stream_ch = stream_factory ? stream_factory->Get(type.c_str(), name.c_str()) : nullptr;
            const std::string role = ReadRoleFromOption(option);

            rapidjson::Document option_doc;
            std::string option_parse_err;
            const bool option_ok = (ParseOptionObject(option, &option_doc, &option_parse_err) == 0 && option_doc.IsObject());

            w.StartObject();
            w.Key("type");
            w.String(type.c_str());
            w.Key("name");
            w.String(name.c_str());
            w.Key("role");
            w.String(role.c_str());
            w.Key("option");
            w.String(option.c_str());
            w.Key("option_json");
            if (option_ok) {
                rapidjson::Document option_only;
                option_only.SetObject();
                auto& alloc = option_only.GetAllocator();
                for (auto it = option_doc.MemberBegin(); it != option_doc.MemberEnd(); ++it) {
                    if (std::string(it->name.GetString()) == "role") continue;
                    rapidjson::Value key_json;
                    key_json.SetString(it->name.GetString(), alloc);
                    rapidjson::Value val_json;
                    val_json.CopyFrom(it->value, alloc);
                    option_only.AddMember(key_json, val_json, alloc);
                }
                const std::string option_json = OptionObjectToJson(option_only);
                w.RawValue(option_json.c_str(), option_json.size(), rapidjson::kObjectType);
            } else {
                w.StartObject();
                w.EndObject();
            }
            w.Key("status");
            w.String(status.c_str());
            w.Key("in_use");
            w.Bool(in_use_count > 0);
            w.Key("capacity");
            w.Uint64(stream_ch ? stream_ch->Capacity() : 0);
            w.Key("size");
            w.Uint64(stream_ch ? stream_ch->Size() : 0);
            w.Key("is_finite");
            w.Bool(stream_ch ? stream_ch->IsFinite() : false);
            w.Key("is_finished");
            w.Bool(stream_ch ? stream_ch->IsFinished() : true);
            w.Key("derived_channels");
            w.StartArray();
            if (stream_ch && stream_ch->IsHubChannel() &&
                IEquals(stream_ch->HubModeHint() ? stream_ch->HubModeHint() : "", "split")) {
                for (size_t i = 0; i < stream_ch->HubPartitionCount(); ++i) {
                    auto partition = stream_ch->HubPartition(i);
                    if (!partition) continue;
                    std::string part_status = "running";
                    if (partition->IsFinished() && partition->IsEmpty()) {
                        part_status = "stopped";
                    } else if (partition->IsFinished()) {
                        part_status = "draining";
                    }
                    w.StartObject();
                    w.Key("index");
                    w.Uint(static_cast<unsigned>(i));
                    w.Key("name");
                    w.String(partition->Name());
                    w.Key("status");
                    w.String(part_status.c_str());
                    w.Key("capacity");
                    w.Uint64(partition->Capacity());
                    w.Key("size");
                    w.Uint64(partition->Size());
                    w.Key("is_finite");
                    w.Bool(partition->IsFinite());
                    w.Key("is_finished");
                    w.Bool(partition->IsFinished());
                    w.EndObject();
                }
            }
            w.EndArray();
            w.EndObject();
        });
    }

    w.EndArray();
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t SchedulerPlugin::HandleAddStreamChannel(const std::string&,
                                                const std::string& req,
                                                std::string& rsp) {
    auto* stream_manager = querier_ ? static_cast<IStreamManager*>(querier_->First(IID_STREAM_MANAGER)) : nullptr;
    if (!stream_manager) {
        rsp = BuildErrorJson("stream manager unavailable");
        return error::UNAVAILABLE;
    }

    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        rsp = BuildErrorJson("invalid request body");
        return error::BAD_REQUEST;
    }

    std::string type;
    std::string name;
    std::string role_raw;
    std::string option_legacy;
    const rapidjson::Value* options_obj = nullptr;
    const rapidjson::Value* cfg = (doc.HasMember("config") && doc["config"].IsObject()) ? &doc["config"] : nullptr;
    auto read_string = [&](const char* key, std::string* out) {
        if (!out) return;
        out->clear();
        if (doc.HasMember(key) && doc[key].IsString()) {
            *out = doc[key].GetString();
            return;
        }
        if (cfg && cfg->HasMember(key) && (*cfg)[key].IsString()) {
            *out = (*cfg)[key].GetString();
        }
    };
    read_string("type", &type);
    read_string("name", &name);
    read_string("role", &role_raw);
    read_string("option", &option_legacy);
    if (doc.HasMember("options") && doc["options"].IsObject()) {
        options_obj = &doc["options"];
    } else if (cfg && cfg->HasMember("options") && (*cfg)["options"].IsObject()) {
        options_obj = &(*cfg)["options"];
    }
    if (type.empty() || name.empty()) {
        rsp = BuildErrorJson("invalid request, expected {\"type\":\"...\",\"name\":\"...\",\"role\":\"...\",\"options\":{...}}");
        return error::BAD_REQUEST;
    }
    std::string role = role_raw.empty() ? "both" : NormalizeStreamRole(role_raw);
    if (role.empty()) {
        rsp = BuildErrorJson("invalid role, expected source|sink|both");
        return error::BAD_REQUEST;
    }
    if (!option_legacy.empty() && role_raw.empty()) {
        role = ReadRoleFromOption(option_legacy);
    }

    auto* builtin_registry = querier_ ? static_cast<IBuiltinRegistry*>(querier_->First(IID_BUILTIN_REGISTRY)) : nullptr;
    if (builtin_registry) {
        StreamChannelTypeDescriptor def;
        if (builtin_registry->FindStreamChannelType(type, &def) != 0) {
            rsp = BuildErrorJson("unsupported stream channel type: " + type);
            return error::BAD_REQUEST;
        }
        bool allowed = def.allowed_roles.empty();
        for (const auto& item : def.allowed_roles) {
            if (NormalizeStreamRole(item) == role) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            rsp = BuildErrorJson("role is not allowed for stream type: " + type);
            return error::BAD_REQUEST;
        }
    }

    std::string option;
    if (options_obj) {
        option = BuildOptionWithRoleJson(options_obj, role);
    } else if (!option_legacy.empty()) {
        rapidjson::Document option_doc;
        std::string parse_err;
        if (ParseOptionObject(option_legacy, &option_doc, &parse_err) != 0 || !option_doc.IsObject()) {
            rsp = BuildErrorJson("invalid option: " + parse_err);
            return error::BAD_REQUEST;
        }
        option = BuildOptionWithRoleJson(&option_doc, role);
    } else {
        option = BuildOptionWithRoleJson(nullptr, role);
    }

    const int rc = stream_manager->AddChannel(ToLowerAscii(type), name, option);
    if (rc != 0) {
        rsp = BuildErrorJson("add stream channel failed: " + type + "." + name);
        return MapStreamManagerErrorToStatus(rc);
    }
    {
        std::lock_guard<std::mutex> lock(stream_channel_refs_mu_);
        stream_channel_versions_[MakeStreamChannelKey(type, name)] += 1;
    }
    rsp = R"({"ok":true})";
    return error::OK;
}

int32_t SchedulerPlugin::HandleModifyStreamChannel(const std::string&,
                                                   const std::string& req,
                                                   std::string& rsp) {
    auto* stream_manager = querier_ ? static_cast<IStreamManager*>(querier_->First(IID_STREAM_MANAGER)) : nullptr;
    if (!stream_manager) {
        rsp = BuildErrorJson("stream manager unavailable");
        return error::UNAVAILABLE;
    }

    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        rsp = BuildErrorJson("invalid request body");
        return error::BAD_REQUEST;
    }

    std::string type;
    std::string name;
    std::string role_raw;
    std::string option_legacy;
    const rapidjson::Value* options_obj = nullptr;
    const rapidjson::Value* cfg = (doc.HasMember("config") && doc["config"].IsObject()) ? &doc["config"] : nullptr;
    auto read_string = [&](const char* key, std::string* out) {
        if (!out) return;
        out->clear();
        if (doc.HasMember(key) && doc[key].IsString()) {
            *out = doc[key].GetString();
            return;
        }
        if (cfg && cfg->HasMember(key) && (*cfg)[key].IsString()) {
            *out = (*cfg)[key].GetString();
        }
    };
    read_string("type", &type);
    read_string("name", &name);
    read_string("role", &role_raw);
    read_string("option", &option_legacy);
    if (doc.HasMember("options") && doc["options"].IsObject()) {
        options_obj = &doc["options"];
    } else if (cfg && cfg->HasMember("options") && (*cfg)["options"].IsObject()) {
        options_obj = &(*cfg)["options"];
    }
    if (type.empty() || name.empty()) {
        rsp = BuildErrorJson("invalid request, expected {\"type\":\"...\",\"name\":\"...\",\"role\":\"...\",\"options\":{...}}");
        return error::BAD_REQUEST;
    }
    std::string role = role_raw.empty() ? "both" : NormalizeStreamRole(role_raw);
    if (role.empty()) {
        rsp = BuildErrorJson("invalid role, expected source|sink|both");
        return error::BAD_REQUEST;
    }
    if (!option_legacy.empty() && role_raw.empty()) {
        role = ReadRoleFromOption(option_legacy);
    }

    auto* builtin_registry = querier_ ? static_cast<IBuiltinRegistry*>(querier_->First(IID_BUILTIN_REGISTRY)) : nullptr;
    if (builtin_registry) {
        StreamChannelTypeDescriptor def;
        if (builtin_registry->FindStreamChannelType(type, &def) != 0) {
            rsp = BuildErrorJson("unsupported stream channel type: " + type);
            return error::BAD_REQUEST;
        }
        bool allowed = def.allowed_roles.empty();
        for (const auto& item : def.allowed_roles) {
            if (NormalizeStreamRole(item) == role) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            rsp = BuildErrorJson("role is not allowed for stream type: " + type);
            return error::BAD_REQUEST;
        }
    }
    std::string option;
    if (options_obj) {
        option = BuildOptionWithRoleJson(options_obj, role);
    } else if (!option_legacy.empty()) {
        rapidjson::Document option_doc;
        std::string parse_err;
        if (ParseOptionObject(option_legacy, &option_doc, &parse_err) != 0 || !option_doc.IsObject()) {
            rsp = BuildErrorJson("invalid option: " + parse_err);
            return error::BAD_REQUEST;
        }
        option = BuildOptionWithRoleJson(&option_doc, role);
    } else {
        option = BuildOptionWithRoleJson(nullptr, role);
    }

    SweepFinishedTaskLeases();
    const std::string key = MakeStreamChannelKey(type, name);
    std::string mutation_reason;
    const int mutation_rc = TryBeginStreamChannelMutation(key, &mutation_reason);
    if (mutation_rc != 0) {
        if (mutation_reason == "source_in_use") {
            rsp = BuildExecutionErrorJson("stream source is in use", ErrorCodeId::kStreamSourceInUse, ErrorStageId::kModify);
            return error::CONFLICT;
        }
        if (mutation_reason == "mutating") {
            rsp = BuildExecutionErrorJson("stream channel is mutating", ErrorCodeId::kStreamChannelMutating, ErrorStageId::kModify);
            return error::CONFLICT;
        }
        rsp = BuildExecutionErrorJson("stream channel is in use", ErrorCodeId::kStreamChannelInUse, ErrorStageId::kModify);
        return error::CONFLICT;
    }
    auto mutation_guard = std::unique_ptr<void, std::function<void(void*)>>(
        reinterpret_cast<void*>(1),
        [this, key](void*) { EndStreamChannelMutation(key); });

    const int rc = stream_manager->ModifyChannel(ToLowerAscii(type), name, option);
    if (rc != 0) {
        rsp = BuildErrorJson("modify stream channel failed: " + type + "." + name);
        return MapStreamManagerErrorToStatus(rc);
    }
    mutation_guard.reset();
    rsp = R"({"ok":true})";
    return error::OK;
}

int32_t SchedulerPlugin::HandleRemoveStreamChannel(const std::string&,
                                                   const std::string& req,
                                                   std::string& rsp) {
    auto* stream_manager = querier_ ? static_cast<IStreamManager*>(querier_->First(IID_STREAM_MANAGER)) : nullptr;
    if (!stream_manager) {
        rsp = BuildErrorJson("stream manager unavailable");
        return error::UNAVAILABLE;
    }

    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() ||
        !doc.HasMember("type") || !doc["type"].IsString() ||
        !doc.HasMember("name") || !doc["name"].IsString()) {
        rsp = BuildErrorJson("invalid request, expected {\"type\":\"...\",\"name\":\"...\"}");
        return error::BAD_REQUEST;
    }
    const std::string type = doc["type"].GetString();
    const std::string name = doc["name"].GetString();

    SweepFinishedTaskLeases();
    const std::string key = MakeStreamChannelKey(type, name);
    std::string mutation_reason;
    const int mutation_rc = TryBeginStreamChannelMutation(key, &mutation_reason);
    if (mutation_rc != 0) {
        if (mutation_reason == "source_in_use") {
            rsp = BuildExecutionErrorJson("stream source is in use", ErrorCodeId::kStreamSourceInUse, ErrorStageId::kRemove);
            return error::CONFLICT;
        }
        if (mutation_reason == "mutating") {
            rsp = BuildExecutionErrorJson("stream channel is mutating", ErrorCodeId::kStreamChannelMutating, ErrorStageId::kRemove);
            return error::CONFLICT;
        }
        rsp = BuildExecutionErrorJson("stream channel is in use", ErrorCodeId::kStreamChannelInUse, ErrorStageId::kRemove);
        return error::CONFLICT;
    }
    auto mutation_guard = std::unique_ptr<void, std::function<void(void*)>>(
        reinterpret_cast<void*>(1),
        [this, key](void*) { EndStreamChannelMutation(key); });

    const int rc = stream_manager->RemoveChannel(ToLowerAscii(type), name);
    if (rc != 0) {
        rsp = BuildErrorJson("remove stream channel failed: " + type + "." + name);
        return MapStreamManagerErrorToStatus(rc);
    }
    mutation_guard.reset();
    rsp = R"({"ok":true})";
    return error::OK;
}

// --- HandlePreviewDataframe ---
// POST /channels/dataframe/preview — Body: {"category":"...","name":"..."} 或 {"name":"..."}
int32_t SchedulerPlugin::HandlePreviewDataframe(const std::string&, const std::string& req, std::string& rsp) {
    // 逻辑链：
    // 1) 解析分页参数并按 category.name 定位目标 dataframe 通道；
    // 2) 依次在托管通道、静态注册通道、命名 dataframe 注册中心中查找；
    // 3) 读取 DataFrame 快照并按页裁剪；
    // 4) 输出 columns/types/data 的前端预览结构。
    auto* ch_registry = querier_ ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY)) : nullptr;
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("name") || !doc["name"].IsString()) {
        rsp = R"({"error":"missing 'name'"})";
        return error::BAD_REQUEST;
    }
    std::string category = "dataframe";
    if (doc.HasMember("category") && doc["category"].IsString()) {
        category = doc["category"].GetString();
    }
    std::string name = doc["name"].GetString();
    int page = 1;
    int page_size = 20;
    if (doc.HasMember("page") && doc["page"].IsInt()) page = doc["page"].GetInt();
    if (doc.HasMember("page_size") && doc["page_size"].IsInt()) page_size = doc["page_size"].GetInt();
    if (page < 1) page = 1;
    if (page_size < 1) page_size = 20;
    if (page_size > 100) page_size = 100;
    std::string key     = category + "." + name;

    // 先在内部通道表查找
    std::shared_ptr<IChannel> managed_holder = FindManagedChannelShared(key);
    IChannel* raw_ch = managed_holder.get();

    // 再去 IQuerier 静态注册通道查找
    if (!raw_ch && querier_) {
        querier_->Traverse(IID_CHANNEL, [&](void* p) -> int {
            auto* ch = static_cast<IChannel*>(p);
            if (IEquals(ch->Category(), category) && std::string(ch->Name()) == name) {
                raw_ch = ch;
                return 1;  // 找到，停止遍历
            }
            return 0;
        });
    }

    if (!raw_ch) {
        if (IEquals(category, "dataframe") && ch_registry) {
            auto named = ch_registry->Get(name.c_str());
            auto* named_df = dynamic_cast<IDataFrameChannel*>(named.get());
            if (named_df) raw_ch = named_df;
        }
    }

    if (!raw_ch) {
        rsp = "{\"error\":\"channel not found: " + key + "\"}";
        return error::NOT_FOUND;
    }

    auto* df_ch = dynamic_cast<IDataFrameChannel*>(raw_ch);
    if (!df_ch) {
        rsp = R"({"error":"not a dataframe channel"})";
        return error::BAD_REQUEST;
    }

    DataFrame data;
    if (df_ch->Read(&data) != 0 || data.RowCount() == 0) {
        rsp = R"({"columns":[],"types":[],"data":[],"rows":0})";
        return error::OK;
    }
    const auto schema = data.GetSchema();
    const int rows = data.RowCount();
    const int start = (page - 1) * page_size;
    const int end = std::min(rows, start + page_size);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("columns");
    w.StartArray();
    for (const auto& f : schema) w.String(f.name.c_str());
    w.EndArray();
    w.Key("types");
    w.StartArray();
    for (const auto& f : schema) w.String(DataTypeName(f.type));
    w.EndArray();
    w.Key("data");
    w.StartArray();
    for (int r = start; r < end; ++r) {
        const auto row = data.GetRow(r);
        w.StartArray();
        for (const auto& v : row) {
            std::visit(
                [&](auto&& val) {
                    using T = std::decay_t<decltype(val)>;
                    if constexpr (std::is_same_v<T, int32_t>) w.Int(val);
                    else if constexpr (std::is_same_v<T, int64_t>) w.Int64(val);
                    else if constexpr (std::is_same_v<T, uint32_t>) w.Uint(val);
                    else if constexpr (std::is_same_v<T, uint64_t>) w.Uint64(val);
                    else if constexpr (std::is_same_v<T, float>) w.Double(val);
                    else if constexpr (std::is_same_v<T, double>) w.Double(val);
                    else if constexpr (std::is_same_v<T, std::string>) w.String(val.c_str());
                    else if constexpr (std::is_same_v<T, std::vector<uint8_t>>)
                        w.String(reinterpret_cast<const char*>(val.data()), val.size());
                    else if constexpr (std::is_same_v<T, bool>) w.Bool(val);
                },
                v);
        }
        w.EndArray();
    }
    w.EndArray();
    w.Key("rows");
    w.Int(rows);
    w.Key("page");
    w.Int(page);
    w.Key("page_size");
    w.Int(page_size);
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}
// --- HandleRefreshOperators ---
int32_t SchedulerPlugin::HandleRefreshOperators(const std::string&, const std::string&, std::string& rsp) {
    if (!querier_) {
        rsp = R"({"error":"querier not initialized"})";
        return error::INTERNAL_ERROR;
    }
    auto* bridge = static_cast<IBridge*>(querier_->First(IID_BRIDGE));
    if (bridge) {
        int rc = bridge->Refresh();
        if (rc == 0) {
            rsp = R"({"status":"refreshed"})";
            return error::OK;
        } else {
            rsp = R"({"error":"refresh failed"})";
            return error::INTERNAL_ERROR;
        }
    } else {
        rsp = R"({"error":"bridge not available"})";
        return error::NOT_FOUND;
    }
}

}  // namespace scheduler
}  // namespace flowsql
