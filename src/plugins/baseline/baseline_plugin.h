/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_BASELINE_PLUGIN_H_
#define _FLOWSQL_PLUGINS_BASELINE_BASELINE_PLUGIN_H_

#include <common/error_code.h>
#include <common/iplugin.h>
#include <framework/interfaces/ibaseline_service.h>

#include <memory>
#include <string>

namespace flowsql {
namespace baseline {

class TaskRegistry;
class RebuildQueue;
class RebuildWorker;
class KeyRiskFusion;

class __attribute__((visibility("default"))) BaselinePlugin : public IPlugin, public IBaselineService {
 public:
    BaselinePlugin();
    ~BaselinePlugin() override;

    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;

    int CreateValueTask(const char* config_json,
                        IBaselineValueTask** out) override;
    int CreateRatioTask(const char* config_json,
                        IBaselineRatioTask** out) override;
    int CreateRelationTask(const char* config_json,
                           IBaselineSourceResolver* resolver,
                           IBaselineRelationTask** out) override;
    void ListTasks(std::function<void(const char* task_id,
                                      const char* task_name,
                                      BaselineTaskKind kind)> cb) override;
    int QueryKeyFusionSnapshotJson(const BaselineStringRef& key,
                                   std::string* out_json) const override;
    int QueryServiceStatsJson(std::string* out_json) const override;

 private:
    IQuerier* querier_ = nullptr;
    std::string option_;
    std::string config_file_;
    bool config_strict_ = true;
    std::unique_ptr<TaskRegistry> task_registry_;
    std::unique_ptr<RebuildQueue> rebuild_queue_;
    std::unique_ptr<RebuildWorker> rebuild_worker_;
    std::unique_ptr<KeyRiskFusion> key_risk_fusion_;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_BASELINE_PLUGIN_H_
