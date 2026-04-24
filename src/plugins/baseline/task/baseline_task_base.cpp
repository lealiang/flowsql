/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "baseline_task_base.h"

#include <common/error_code.h>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "task_registry.h"

namespace flowsql {
namespace baseline {

BaselineTaskBase::BaselineTaskBase(TaskRegistry* registry,
                                   RebuildQueue* rebuild_queue,
                                   std::string task_id,
                                   BaselineTaskKind kind,
                                   std::string task_name,
                                   std::string config_json)
    : registry_(registry),
      rebuild_queue_(rebuild_queue),
      task_id_(std::move(task_id)),
      kind_(kind),
      task_name_(std::move(task_name)),
      config_json_(std::move(config_json)) {}

const char* BaselineTaskBase::Id() const { return task_id_.c_str(); }
const char* BaselineTaskBase::Name() const { return task_name_.c_str(); }
BaselineTaskKind BaselineTaskBase::Kind() const { return kind_; }
const char* BaselineTaskBase::ConfigJson() const { return config_json_.c_str(); }
const std::string& BaselineTaskBase::TaskId() const { return task_id_; }

int BaselineTaskBase::QueryTaskSnapshotJson(std::string* out_json) const {
    if (!out_json) return error::BAD_REQUEST;

    std::lock_guard<std::mutex> lock(mutex_);
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("task_id");
    writer.String(task_id_.c_str());
    writer.Key("name");
    writer.String(task_name_.c_str());
    writer.Key("kind");
    writer.String(KindName(kind_));
    writer.Key("closed");
    writer.Bool(closed_);
    writer.EndObject();
    *out_json = buf.GetString();
    return error::OK;
}

int BaselineTaskBase::QuerySeriesSnapshotJson(const BaselineStringRef& key,
                                              std::string* out_json) const {
    if (!out_json) return error::BAD_REQUEST;

    std::lock_guard<std::mutex> lock(mutex_);
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("task_id");
    writer.String(task_id_.c_str());
    writer.Key("key");
    const std::string key_copy = CopyStringRef(key);
    writer.String(key_copy.c_str());
    writer.Key("status");
    writer.String("not_ready");
    writer.EndObject();
    *out_json = buf.GetString();
    return error::OK;
}

int BaselineTaskBase::RequestRebuild(const BaselineStringRef&, BaselineRebuildReason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (EnsureOpenLocked() != error::OK) return error::UNAVAILABLE;
    return error::UNAVAILABLE;
}

int BaselineTaskBase::Close() {
    std::shared_ptr<BaselineTaskBase> self = shared_from_this();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return error::OK;
        closed_ = true;
        OnClosingLocked();
    }

    if (registry_) registry_->Unregister(task_id_, this);
    return error::OK;
}

int BaselineTaskBase::EnsureOpenLocked() const {
    return closed_ ? error::UNAVAILABLE : error::OK;
}

std::string BaselineTaskBase::CopyStringRef(const BaselineStringRef& ref) {
    if (!ref.data || ref.size == 0) return "";
    return std::string(ref.data, ref.size);
}

void BaselineTaskBase::OnClosingLocked() {}

const char* BaselineTaskBase::KindName(BaselineTaskKind kind) {
    switch (kind) {
        case BaselineTaskKind::kValue:
            return "value";
        case BaselineTaskKind::kRatio:
            return "ratio";
        case BaselineTaskKind::kRelation:
            return "relation";
    }
    return "unknown";
}

}  // namespace baseline
}  // namespace flowsql
