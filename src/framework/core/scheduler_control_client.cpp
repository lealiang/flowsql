/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "framework/core/scheduler_control_client.h"

#include "framework/core/json_error_builder.h"

namespace flowsql {

ISchedulerControlService* SchedulerControlClient::Acquire(std::string* err_rsp) const {
    if (!querier_) {
        if (err_rsp) *err_rsp = BuildErrorJson("scheduler control service unavailable");
        return nullptr;
    }
    auto* control = static_cast<ISchedulerControlService*>(querier_->First(IID_SCHEDULER_CONTROL_SERVICE));
    if (!control) {
        if (err_rsp) *err_rsp = BuildErrorJson("scheduler control service unavailable");
        return nullptr;
    }
    return control;
}

int32_t SchedulerControlClient::ClassifySql(const std::string& req_json, std::string* rsp_json) const {
    if (!rsp_json) return error::INTERNAL_ERROR;
    auto* control = Acquire(rsp_json);
    if (!control) return error::UNAVAILABLE;
    return control->ClassifySql(req_json, rsp_json);
}

int32_t SchedulerControlClient::ExecuteBatch(const std::string& req_json, std::string* rsp_json) const {
    if (!rsp_json) return error::INTERNAL_ERROR;
    auto* control = Acquire(rsp_json);
    if (!control) return error::UNAVAILABLE;
    return control->ExecuteBatch(req_json, rsp_json);
}

int32_t SchedulerControlClient::ExecuteStream(const std::string& req_json, std::string* rsp_json) const {
    if (!rsp_json) return error::INTERNAL_ERROR;
    auto* control = Acquire(rsp_json);
    if (!control) return error::UNAVAILABLE;
    return control->ExecuteStream(req_json, rsp_json);
}

int32_t SchedulerControlClient::StopStream(const std::string& req_json, std::string* rsp_json) const {
    if (!rsp_json) return error::INTERNAL_ERROR;
    auto* control = Acquire(rsp_json);
    if (!control) return error::UNAVAILABLE;
    return control->StopStream(req_json, rsp_json);
}

int32_t SchedulerControlClient::QueryStreamStatus(const std::string& req_json, std::string* rsp_json) const {
    if (!rsp_json) return error::INTERNAL_ERROR;
    auto* control = Acquire(rsp_json);
    if (!control) return error::UNAVAILABLE;
    return control->QueryStreamStatus(req_json, rsp_json);
}

int32_t RouterBackedSchedulerControlService::Dispatch(const char* uri,
                                                      const std::string& req_json,
                                                      std::string* rsp_json) const {
    if (!rsp_json) return error::INTERNAL_ERROR;
    if (!router_) {
        *rsp_json = BuildErrorJson("scheduler route not found");
        return error::UNAVAILABLE;
    }

    fnRouterHandler handler;
    router_->EnumRoutes([&](const RouteItem& item) {
        if (item.method == "POST" && item.uri == uri) {
            handler = item.handler;
        }
    });
    if (!handler) {
        *rsp_json = BuildErrorJson(std::string("scheduler route not found: ") + uri);
        return error::UNAVAILABLE;
    }
    return handler(uri, req_json, *rsp_json);
}

int32_t RouterBackedSchedulerControlService::ClassifySql(const std::string& req_json, std::string* rsp_json) {
    return Dispatch("/scheduler/sql/classify", req_json, rsp_json);
}

int32_t RouterBackedSchedulerControlService::ExecuteBatch(const std::string& req_json, std::string* rsp_json) {
    return Dispatch("/scheduler/batch/execute", req_json, rsp_json);
}

int32_t RouterBackedSchedulerControlService::ExecuteStream(const std::string& req_json, std::string* rsp_json) {
    return Dispatch("/scheduler/stream/execute", req_json, rsp_json);
}

int32_t RouterBackedSchedulerControlService::StopStream(const std::string& req_json, std::string* rsp_json) {
    return Dispatch("/scheduler/stream/stop", req_json, rsp_json);
}

int32_t RouterBackedSchedulerControlService::QueryStreamStatus(const std::string& req_json, std::string* rsp_json) {
    return Dispatch("/scheduler/stream/status", req_json, rsp_json);
}

}  // namespace flowsql
