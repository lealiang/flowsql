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

namespace flowsql {
namespace baseline {

class TaskRegistry;
class RebuildQueue;

class BaselineTaskBase : public std::enable_shared_from_this<BaselineTaskBase> {
 public:
    BaselineTaskBase(TaskRegistry* registry,
                     RebuildQueue* rebuild_queue,
                     std::string task_id,
                     BaselineTaskKind kind,
                     std::string task_name,
                     std::string config_json);
    virtual ~BaselineTaskBase() = default;

    const char* Id() const;
    const char* Name() const;
    BaselineTaskKind Kind() const;
    const char* ConfigJson() const;
    const std::string& TaskId() const;

    int QueryTaskSnapshotJson(std::string* out_json) const;
    int QuerySeriesSnapshotJson(const BaselineStringRef& key, std::string* out_json) const;
    int RequestRebuild(const BaselineStringRef& key, BaselineRebuildReason reason);
    int Close();

 protected:
    int EnsureOpenLocked() const;
    static std::string CopyStringRef(const BaselineStringRef& ref);
    virtual void OnClosingLocked();

    mutable std::mutex mutex_;
    RebuildQueue* rebuild_queue_ = nullptr;

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
