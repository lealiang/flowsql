/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "scheduler_plugin.h"

#include <framework/core/json_error_builder.h>
#include <framework/core/sql_text_splitter.h>

#include "scheduler_internal_utils.h"

namespace flowsql {
namespace scheduler {

namespace {

constexpr size_t kDefaultMaxGroupNodes = 64;
constexpr int kDefaultShareSetReadyTimeoutS = 30;

}  // namespace

int32_t SchedulerPlugin::ParseStreamGroupExecuteRequest(const rapidjson::Document& doc,
                                                        StreamGroupExecuteRequest* out,
                                                        std::string* err_rsp) {
    if (!out || !err_rsp) return error::INTERNAL_ERROR;

    if (!doc.HasMember("group_mode") || !doc["group_mode"].IsString()) {
        *err_rsp = BuildExecutionErrorJson(
            "group_mode must be provided for group execution",
            ErrorCodeId::kStreamGroupModeInvalid,
            ErrorStageId::kRequest);
        return error::BAD_REQUEST;
    }
    const std::string group_mode = ToLowerAscii(doc["group_mode"].GetString());
    if (group_mode != "dag") {
        *err_rsp = BuildExecutionErrorJson(
            "only group_mode=dag is supported",
            ErrorCodeId::kStreamGroupModeInvalid,
            ErrorStageId::kRequest);
        return error::BAD_REQUEST;
    }

    if (doc.HasMember("dag") ||
        doc.HasMember("nodes") ||
        doc.HasMember("source_share_sets") ||
        doc.HasMember("sql") ||
        doc.HasMember("sqls")) {
        *err_rsp = BuildExecutionErrorJson(
            "group execution accepts only sql_text/group_mode/timeout fields",
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kRequest);
        return error::BAD_REQUEST;
    }

    if (!doc.HasMember("sql_text") || !doc["sql_text"].IsString()) {
        *err_rsp = BuildExecutionErrorJson(
            "group execution requires sql_text",
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kRequest);
        return error::BAD_REQUEST;
    }

    int timeout_s = 0;
    if (doc.HasMember("timeout_s")) {
        if (!doc["timeout_s"].IsInt()) {
            *err_rsp = BuildExecutionErrorJson(
                "timeout_s must be integer",
                ErrorCodeId::kStreamGroupSqlTextInvalid,
                ErrorStageId::kRequest);
            return error::BAD_REQUEST;
        }
        timeout_s = doc["timeout_s"].GetInt();
        if (timeout_s < 0) {
            *err_rsp = BuildExecutionErrorJson(
                "timeout_s must be >= 0",
                ErrorCodeId::kStreamGroupSqlTextInvalid,
                ErrorStageId::kRequest);
            return error::BAD_REQUEST;
        }
        if (timeout_s > max_stream_group_timeout_s_) {
            *err_rsp = BuildExecutionErrorJson(
                "timeout_s exceeds max_stream_group_timeout_s: " +
                    std::to_string(max_stream_group_timeout_s_),
                ErrorCodeId::kStreamGroupSqlTextInvalid,
                ErrorStageId::kRequest);
            return error::BAD_REQUEST;
        }
    }

    int share_set_ready_timeout_s = kDefaultShareSetReadyTimeoutS;
    if (doc.HasMember("share_set_ready_timeout_s")) {
        if (!doc["share_set_ready_timeout_s"].IsInt()) {
            *err_rsp = BuildExecutionErrorJson(
                "share_set_ready_timeout_s must be integer",
                ErrorCodeId::kStreamGroupSqlTextInvalid,
                ErrorStageId::kRequest);
            return error::BAD_REQUEST;
        }
        share_set_ready_timeout_s = doc["share_set_ready_timeout_s"].GetInt();
        if (share_set_ready_timeout_s <= 0) {
            *err_rsp = BuildExecutionErrorJson(
                "share_set_ready_timeout_s must be > 0",
                ErrorCodeId::kStreamGroupSqlTextInvalid,
                ErrorStageId::kRequest);
            return error::BAD_REQUEST;
        }
    }
    if (timeout_s > 0 && share_set_ready_timeout_s > timeout_s) {
        share_set_ready_timeout_s = timeout_s;
    }

    SqlTextSplitError split_err;
    std::vector<std::string> sqls;
    if (SplitSqlText(doc["sql_text"].GetString(), &sqls, &split_err) != 0) {
        std::string err = "invalid sql_text";
        if (!split_err.message.empty()) {
            err += ": " + split_err.message;
        }
        *err_rsp = BuildExecutionErrorWithSqlIndexJson(
            err,
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kRequest,
            split_err.statement_index);
        return error::BAD_REQUEST;
    }

    if (sqls.size() < 2) {
        *err_rsp = BuildExecutionErrorJson(
            "group execution requires at least two SQL statements",
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kRequest);
        return error::BAD_REQUEST;
    }
    if (sqls.size() > kDefaultMaxGroupNodes) {
        *err_rsp = BuildExecutionErrorJson(
            "group nodes exceed max_group_nodes",
            ErrorCodeId::kStreamGroupDagTooLarge,
            ErrorStageId::kDagValidate);
        return error::BAD_REQUEST;
    }

    out->timeout_s = timeout_s;
    out->share_set_ready_timeout_s = share_set_ready_timeout_s;
    out->sqls = std::move(sqls);
    return error::OK;
}

}  // namespace scheduler
}  // namespace flowsql
