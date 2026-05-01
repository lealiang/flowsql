/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_PLUGINS_BASELINE_TASK_BASELINE_TASK_BASE_H_
#define _FLOWSQL_PLUGINS_BASELINE_TASK_BASELINE_TASK_BASE_H_

#include <framework/interfaces/ibaseline_service.h>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace flowsql {
namespace baseline {

class TaskRegistry;

class BaselineTaskBase : public std::enable_shared_from_this<BaselineTaskBase> {
 public:
    BaselineTaskBase(TaskRegistry* registry,
                     std::string task_id,
                     BaselineTaskKind kind,
                     std::string task_name,
                     std::string config_json);
    virtual ~BaselineTaskBase() = default;

    const char* Id() const;
    const char* Name() const;
    BaselineTaskKind Kind() const;
    const std::string& TaskId() const;

    BaselineSerializationResult ExportConfig(BaselineSerializationFormat format) const;
    BaselineSerializationResult QueryTaskSnapshot(BaselineSerializationFormat format) const;
    BaselineSerializationResult QuerySeriesSnapshot(std::string_view series_key,
                                                    BaselineSerializationFormat format) const;
    BaselineStatus Close();

 protected:
    BaselineStatus EnsureOpenLocked() const;
    static BaselineSerializationResult UnsupportedFormatResult(
        BaselineSerializationFormat format);
    virtual void OnClosingLocked();

    mutable std::mutex mutex_;

 private:
    static const char* KindName(BaselineTaskKind kind);

    TaskRegistry* registry_ = nullptr;
    std::string task_id_;
    BaselineTaskKind kind_ = BaselineTaskKind::kValue;
    std::string task_name_;
    std::string config_json_;
    bool closed_ = false;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_TASK_BASELINE_TASK_BASE_H_
