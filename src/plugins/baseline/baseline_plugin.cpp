/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "baseline_plugin.h"

#include <common/iplugin.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "config_parser.h"
#include "fusion/key_risk_fusion.h"
#include "rebuild/rebuild_queue.h"
#include "rebuild/rebuild_worker.h"
#include "solver/solver_backend.h"
#include "task/relation_task.h"
#include "task/task_registry.h"
#include "task/ratio_task.h"
#include "task/value_task.h"

namespace flowsql {
namespace baseline {

BaselinePlugin::BaselinePlugin()
    : task_registry_(std::make_unique<TaskRegistry>()),
      rebuild_queue_(std::make_unique<RebuildQueue>()),
      rebuild_worker_(std::make_unique<RebuildWorker>(rebuild_queue_.get())),
      key_risk_fusion_(std::make_unique<KeyRiskFusion>()) {}

BaselinePlugin::~BaselinePlugin() = default;

int BaselinePlugin::Option(const char* arg) {
    option_ = arg ? arg : "";
    return error::OK;
}

int BaselinePlugin::Load(IQuerier* querier) {
    querier_ = querier;
    (void)SolverBackend::IsAvailable();
    return error::OK;
}

int BaselinePlugin::Unload() {
    Stop();
    task_registry_ = std::make_unique<TaskRegistry>();
    rebuild_queue_ = std::make_unique<RebuildQueue>();
    rebuild_worker_ = std::make_unique<RebuildWorker>(rebuild_queue_.get());
    key_risk_fusion_ = std::make_unique<KeyRiskFusion>();
    querier_ = nullptr;
    return error::OK;
}

int BaselinePlugin::Start() {
    if (!rebuild_worker_) {
        rebuild_queue_ = std::make_unique<RebuildQueue>();
        rebuild_worker_ = std::make_unique<RebuildWorker>(rebuild_queue_.get());
    }
    if (!key_risk_fusion_) key_risk_fusion_ = std::make_unique<KeyRiskFusion>();
    return rebuild_worker_->Start();
}

int BaselinePlugin::Stop() {
    if (task_registry_) {
        const auto tasks = task_registry_->Snapshot();
        for (const auto& task : tasks) {
            if (task) task->Close();
        }
    }
    if (rebuild_worker_) rebuild_worker_->Stop();
    rebuild_queue_ = std::make_unique<RebuildQueue>();
    rebuild_worker_ = std::make_unique<RebuildWorker>(rebuild_queue_.get());
    key_risk_fusion_ = std::make_unique<KeyRiskFusion>();
    return error::OK;
}

int BaselinePlugin::CreateValueTask(const char* config_json,
                                    IBaselineValueTask** out) {
    if (!out) return error::BAD_REQUEST;
    *out = nullptr;

    BaselineTaskSpec spec;
    std::string err;
    int rc = ConfigParser::ParseValueTask(config_json, &spec, &err);
    if (rc != error::OK) return rc;

    auto task = std::make_shared<BaselineValueTask>(
        task_registry_.get(),
        rebuild_queue_.get(),
        task_registry_->AllocateTaskId(BaselineTaskKind::kValue),
        spec,
        key_risk_fusion_.get());
    rc = task_registry_->Register(task);
    if (rc != error::OK) return rc;

    *out = task.get();
    return error::OK;
}

int BaselinePlugin::CreateRatioTask(const char* config_json,
                                    IBaselineRatioTask** out) {
    if (!out) return error::BAD_REQUEST;
    *out = nullptr;

    BaselineTaskSpec spec;
    std::string err;
    int rc = ConfigParser::ParseRatioTask(config_json, &spec, &err);
    if (rc != error::OK) return rc;

    auto task = std::make_shared<BaselineRatioTask>(
        task_registry_.get(),
        rebuild_queue_.get(),
        task_registry_->AllocateTaskId(BaselineTaskKind::kRatio),
        spec,
        key_risk_fusion_.get());
    rc = task_registry_->Register(task);
    if (rc != error::OK) return rc;

    *out = task.get();
    return error::OK;
}

int BaselinePlugin::CreateRelationTask(const char* config_json,
                                       IBaselineSourceResolver* resolver,
                                       IBaselineRelationTask** out) {
    if (!out) return error::BAD_REQUEST;
    *out = nullptr;

    RelationTaskCreateSpec create_spec;
    std::string err;
    int rc = ConfigParser::ParseRelationTask(config_json, &create_spec, &err);
    if (rc != error::OK) return rc;

    const std::string task_id =
        task_registry_->AllocateTaskId(BaselineTaskKind::kRelation);
    create_spec.task_spec.task_id = task_id;
    auto task = std::make_shared<BaselineRelationTask>(
        task_registry_.get(),
        rebuild_queue_.get(),
        task_id,
        create_spec.task_spec,
        create_spec.clock_spec,
        create_spec.event_calendar_spec,
        resolver,
        key_risk_fusion_.get());
    rc = task_registry_->Register(task);
    if (rc != error::OK) return rc;

    *out = task.get();
    return error::OK;
}

void BaselinePlugin::ListTasks(std::function<void(const char* task_id,
                                                  const char* task_name,
                                                  BaselineTaskKind kind)> cb) {
    if (!task_registry_ || !cb) return;
    task_registry_->List(std::move(cb));
}

int BaselinePlugin::QueryKeyFusionSnapshotJson(const BaselineStringRef& key,
                                               std::string* out_json) const {
    if (!key_risk_fusion_) return error::UNAVAILABLE;
    return key_risk_fusion_->QueryKeyFusionSnapshotJson(key, out_json);
}

int BaselinePlugin::QueryServiceStatsJson(std::string* out_json) const {
    if (!out_json) return error::BAD_REQUEST;

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("task_count");
    writer.Uint64(task_registry_ ? task_registry_->Size() : 0);
    writer.Key("rebuild_queue_depth");
    writer.Uint64(rebuild_queue_ ? rebuild_queue_->Size() : 0);
    writer.Key("rebuild_worker_running");
    writer.Bool(rebuild_worker_ && rebuild_worker_->Running());
    writer.Key("key_fusion_key_count");
    writer.Uint64(key_risk_fusion_ ? key_risk_fusion_->KeyCount() : 0);
    writer.EndObject();
    *out_json = buf.GetString();
    return error::OK;
}

}  // namespace baseline
}  // namespace flowsql

BEGIN_PLUGIN_REGIST(flowsql::baseline::BaselinePlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
    ____INTERFACE(flowsql::IID_BASELINE_SERVICE, flowsql::IBaselineService)
END_PLUGIN_REGIST()
