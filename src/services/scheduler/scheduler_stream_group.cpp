/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "scheduler_plugin.h"

#include <functional>
#include <memory>
#include <string>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace flowsql {
namespace scheduler {

// 逻辑链：
// 1) 解析与校验 group 请求；
// 2) 构建并校验 DAG 计划；
// 3) 申请租约并装配 share set/回调运行时；
// 4) 注册并启动 group，汇总提交响应。
int32_t SchedulerPlugin::HandleStreamExecuteGroup(const rapidjson::Document& doc, std::string& rsp) {
    StreamGroupExecuteRequest req;
    {
        std::string err_rsp;
        const int32_t rc = ParseStreamGroupExecuteRequest(doc, &req, &err_rsp);
        if (rc != error::OK) {
            rsp = std::move(err_rsp);
            return rc;
        }
    }

    StreamGroupBuildArtifacts build;
    {
        std::string err_rsp;
        const int32_t rc = BuildStreamGroupPlan(req, &build, &err_rsp);
        if (rc != error::OK) {
            rsp = std::move(err_rsp);
            return rc;
        }
    }
    {
        std::string err_rsp;
        const int32_t rc = ValidateStreamGroupPlan(build, &err_rsp);
        if (rc != error::OK) {
            rsp = std::move(err_rsp);
            return rc;
        }
    }

    const std::string runtime_task_id = NextStreamTaskId();
    {
        std::string err_rsp;
        const int32_t rc = AcquireStreamGroupLeases(runtime_task_id, build, &err_rsp);
        if (rc != error::OK) {
            rsp = std::move(err_rsp);
            return rc;
        }
    }

    bool release_group_lease_on_fail = true;
    auto group_lease_guard = std::unique_ptr<void, std::function<void(void*)>>(
        reinterpret_cast<void*>(1),
        [this, &runtime_task_id, &release_group_lease_on_fail](void*) {
            if (release_group_lease_on_fail) {
                ReleaseStreamTaskLeases(runtime_task_id);
            }
        });

    StreamGroupRuntimeArtifacts runtime_build;
    std::function<void()> cleanup_local_resources;
    {
        std::string err_rsp;
        const int32_t rc = PrepareStreamGroupRuntimeResources(runtime_task_id,
                                                              build,
                                                              &runtime_build,
                                                              &cleanup_local_resources,
                                                              &err_rsp);
        if (rc != error::OK) {
            rsp = std::move(err_rsp);
            return rc;
        }
    }

    std::shared_ptr<StreamTaskGroup> group = BuildStreamGroupObject(runtime_task_id,
                                                                     req,
                                                                     build,
                                                                     runtime_build,
                                                                     nullptr);

    {
        std::string err_rsp;
        const int32_t rc = RegisterAndStartStreamGroup(runtime_task_id,
                                                       build,
                                                       runtime_build,
                                                       group,
                                                       req.share_set_ready_timeout_s,
                                                       cleanup_local_resources,
                                                       &err_rsp);
        if (rc != error::OK) {
            rsp = std::move(err_rsp);
            return rc;
        }
    }

    release_group_lease_on_fail = false;
    group_lease_guard.reset();

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("status");
    w.String("submitted");
    w.Key("task_id");
    w.String(runtime_task_id.c_str());
    w.Key("runtime_task_id");
    w.String(runtime_task_id.c_str());
    w.Key("runtime_kind");
    w.String("group");
    w.Key("group_mode");
    w.String("dag");
    w.Key("node_count");
    w.Uint(static_cast<unsigned>(build.plans.size()));
    w.Key("share_set_count");
    w.Uint(static_cast<unsigned>(build.share_set_plans.size()));
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

}  // namespace scheduler
}  // namespace flowsql
