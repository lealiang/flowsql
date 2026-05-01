/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_TASK_VALUE_TASK_H_
#define _FLOWSQL_PLUGINS_BASELINE_TASK_VALUE_TASK_H_

#include <memory>
#include <string>
#include <string_view>

#include "baseline_task_base.h"
#include "bootstrap_task_store.h"
#include "plugins/baseline/model/event_calendar_matcher.h"
#include "plugins/baseline/model/task_spec.h"

namespace flowsql {
namespace baseline {

class TaskRegistry;

class BaselineValueTask final : public IBaselineValueTask, public BaselineTaskBase {
 public:
    BaselineValueTask(TaskRegistry* registry,
                      std::string task_id,
                      std::string task_name,
                      std::string config_content,
                      BaselineTaskSpec spec,
                      std::shared_ptr<const CompiledEventCalendar> compiled_event_calendar);

    const char* Id() const override;
    const char* Name() const override;
    BaselineTaskKind Kind() const override;

    BaselineSerializationResult ExportConfig(
        BaselineSerializationFormat format) const override;
    BaselineSerializationResult QueryTaskSnapshot(
        BaselineSerializationFormat format) const override;
    BaselineSerializationResult QuerySeriesSnapshot(
        std::string_view series_key,
        BaselineSerializationFormat format) const override;
    BaselineStatus Close() override;

    BootstrapTrainResult Bootstrap(const ValueBootstrapInput& input) override;
    BootstrapPrediction PredictBootstrap(
        std::string_view series_key,
        int64_t bucket_id,
        const BootstrapPredictionOptions& options) const override;
    BaselineSerializationResult ExportBootstrapArtifact(
        BaselineSerializationFormat format) const override;
    BaselineStatus LoadBootstrapArtifact(
        std::string_view content,
        BaselineSerializationFormat format) override;
    BaselineSerializationResult ExportBootstrapSeed(
        BaselineSerializationFormat format) const override;

 private:
    BaselineTaskSpec spec_;
    std::shared_ptr<const CompiledEventCalendar> compiled_event_calendar_;
    BootstrapArtifactStore artifacts_by_series_;
    BootstrapSeedStore seeds_by_series_;
    BootstrapEngine bootstrap_engine_;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_TASK_VALUE_TASK_H_
