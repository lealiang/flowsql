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

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "config/runtime_config.h"
#include "config_parser.h"
#include "fusion/key_risk_fusion.h"
#include "model/event_calendar_matcher.h"
#include "rebuild/rebuild_queue.h"
#include "rebuild/rebuild_worker.h"
#include "solver/solver_backend.h"
#include "task/relation_task.h"
#include "task/task_registry.h"
#include "task/ratio_task.h"
#include "task/value_task.h"

namespace flowsql {
namespace baseline {

namespace {

int CompileStaticTaskCalendar(const BaselineTaskSpec& spec,
                              std::shared_ptr<const CompiledEventCalendar>* out_calendar) {
    if (!out_calendar) return error::BAD_REQUEST;
    out_calendar->reset();
    if (!spec.event_calendar_spec.has_value()) return error::OK;

    CompiledEventCalendar compiled;
    std::string err;
    const int rc = CompileEventCalendar(*spec.event_calendar_spec, spec, &compiled, &err);
    if (rc != error::OK) return rc;
    *out_calendar = std::make_shared<CompiledEventCalendar>(std::move(compiled));
    return error::OK;
}

int ValidateRelationTaskCalendar(const RelationTaskCreateSpec& create_spec) {
    if (!create_spec.event_calendar_spec.has_value()) return error::OK;

    BaselineTaskSpec task_spec;
    task_spec.feature = create_spec.task_spec.feature_base;
    task_spec.delta = create_spec.clock_spec.delta;
    task_spec.tz = create_spec.clock_spec.tz;

    CompiledEventCalendar compiled;
    std::string err;
    return CompileEventCalendar(*create_spec.event_calendar_spec, task_spec, &compiled, &err);
}

}  // namespace

BaselinePlugin::BaselinePlugin()
    : task_registry_(std::make_unique<TaskRegistry>()),
      rebuild_queue_(std::make_unique<RebuildQueue>()),
      rebuild_worker_(std::make_unique<RebuildWorker>(rebuild_queue_.get())),
      key_risk_fusion_(std::make_unique<KeyRiskFusion>()) {}

BaselinePlugin::~BaselinePlugin() = default;

int BaselinePlugin::Option(const char* arg) {
    option_ = arg ? arg : "";
    config_file_.clear();
    config_strict_ = true;

    if (option_.empty()) return error::OK;
    std::size_t pos = 0;
    while (pos < option_.size()) {
        const std::size_t eq = option_.find('=', pos);
        if (eq == std::string::npos) break;
        std::size_t end = option_.find(';', eq);
        if (end == std::string::npos) end = option_.size();

        const std::string key = option_.substr(pos, eq - pos);
        const std::string value = option_.substr(eq + 1, end - eq - 1);
        if (key == "config_file") {
            config_file_ = value;
        } else if (key == "strict") {
            std::string lowered = value;
            std::transform(lowered.begin(),
                           lowered.end(),
                           lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lowered == "1" || lowered == "true" || lowered == "yes") {
                config_strict_ = true;
            } else if (lowered == "0" || lowered == "false" || lowered == "no") {
                config_strict_ = false;
            } else {
                return error::BAD_REQUEST;
            }
        }
        pos = end < option_.size() ? end + 1 : option_.size();
    }
    return error::OK;
}

int BaselinePlugin::Load(IQuerier* querier) {
    std::string err;
    const int rc =
        LoadBaselineRuntimeConfigFromYaml(config_file_, config_strict_, &err);
    if (rc != error::OK) {
        if (!err.empty()) {
            std::fprintf(stderr, "[baseline] load runtime config failed: %s\n", err.c_str());
        }
        return rc;
    }

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
    ResetBaselineRuntimeConfig();
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

    std::shared_ptr<const CompiledEventCalendar> compiled_event_calendar;
    rc = CompileStaticTaskCalendar(spec, &compiled_event_calendar);
    if (rc != error::OK) return rc;

    auto task = std::make_shared<BaselineValueTask>(
        task_registry_.get(),
        rebuild_queue_.get(),
        task_registry_->AllocateTaskId(BaselineTaskKind::kValue),
        spec,
        compiled_event_calendar,
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

    std::shared_ptr<const CompiledEventCalendar> compiled_event_calendar;
    rc = CompileStaticTaskCalendar(spec, &compiled_event_calendar);
    if (rc != error::OK) return rc;

    auto task = std::make_shared<BaselineRatioTask>(
        task_registry_.get(),
        rebuild_queue_.get(),
        task_registry_->AllocateTaskId(BaselineTaskKind::kRatio),
        spec,
        compiled_event_calendar,
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
    rc = ValidateRelationTaskCalendar(create_spec);
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
    writer.Key("key_fusion_idle_prune_bucket_gap");
    writer.Int64(key_risk_fusion_ ? key_risk_fusion_->IdlePruneBucketGap() : 0);
    writer.Key("key_fusion_pruned_key_count_total");
    writer.Uint64(key_risk_fusion_ ? key_risk_fusion_->PrunedKeyCount() : 0);
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
