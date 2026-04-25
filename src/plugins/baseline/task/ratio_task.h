/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_TASK_RATIO_TASK_H_
#define _FLOWSQL_PLUGINS_BASELINE_TASK_RATIO_TASK_H_

#include <memory>

#include "baseline_task_base.h"
#include "plugins/baseline/detector/ratio_detector_core.h"
#include "plugins/baseline/model/event_calendar_matcher.h"
#include "plugins/baseline/model/task_spec.h"

namespace flowsql {
namespace baseline {

class TaskRegistry;
class RebuildQueue;
class RebuildTaskRuntime;
class KeyRiskFusion;
struct RebuildRequest;
struct RatioHistoryBinding;

class BaselineRatioTask final : public IBaselineRatioTask, public BaselineTaskBase {
 public:
    BaselineRatioTask(TaskRegistry* registry,
                      RebuildQueue* rebuild_queue,
                      std::string task_id,
                      const BaselineTaskSpec& spec,
                      std::shared_ptr<const CompiledEventCalendar> compiled_event_calendar,
                      KeyRiskFusion* key_risk_fusion);

    const char* Id() const override;
    const char* Name() const override;
    BaselineTaskKind Kind() const override;
    const char* ConfigJson() const override;

    int QueryTaskSnapshotJson(std::string* out_json) const override;
    int QuerySeriesSnapshotJson(const BaselineStringRef& key,
                                std::string* out_json) const override;
    int RequestRebuild(const BaselineStringRef& key,
                       BaselineRebuildReason reason) override;
    int Close() override;

    int SetHistoryReader(IBaselineRatioHistoryReader* reader) override;
    int SubmitObservation(const RatioObservation& obs, DetectorResult* out) override;

 protected:
    void OnClosingLocked() override;

 private:
    int ExecuteRebuild(const RebuildRequest& request);

    BaselineTaskSpec spec_;
    std::shared_ptr<const CompiledEventCalendar> compiled_event_calendar_;
    std::shared_ptr<RatioDetectorCore> core_;
    std::shared_ptr<RatioHistoryBinding> history_binding_;
    std::shared_ptr<RebuildTaskRuntime> rebuild_runtime_;
    KeyRiskFusion* key_risk_fusion_ = nullptr;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_TASK_RATIO_TASK_H_
