/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "scheduler_plugin.h"

#include <cerrno>

#include <framework/core/json_error_builder.h>

#include "scheduler_internal_utils.h"

namespace flowsql {
namespace scheduler {

int32_t SchedulerPlugin::AcquireSharedSourceSubscription(StreamExecutionPlan* plan,
                                                         std::shared_ptr<IStreamChannel>* source_override,
                                                         std::string* err_rsp) {
    if (!plan || !source_override || !err_rsp) return error::INTERNAL_ERROR;
    source_override->reset();
    err_rsp->clear();

    if (plan->skip_lease_acquire) {
        return error::OK;
    }
    if (plan->shared_hub_key.empty()) {
        return error::OK;
    }
    if (!plan->source) {
        *err_rsp = BuildExecutionErrorJson(
            "shared source attach failed: source is null",
            ErrorCodeId::kSharedSourceHubCreateFailed,
            ErrorStageId::kSourceResolve);
        return error::INTERNAL_ERROR;
    }

    std::shared_ptr<SharedSourceHub> hub;
    bool created = false;
    {
        std::lock_guard<std::mutex> lock(shared_hubs_mu_);
        auto it = shared_hubs_.find(plan->shared_hub_key);
        if (it != shared_hubs_.end()) {
            hub = it->second;
        } else {
            if (shared_hubs_.size() >= max_shared_hubs_) {
                *err_rsp = BuildExecutionErrorJson(
                    "shared source hub count exceeded max_shared_hubs",
                    ErrorCodeId::kSharedSourceHubCreateFailed,
                    ErrorStageId::kSourceResolve);
                return error::CONFLICT;
            }
            SharedHubOptions opts;
            opts.queue_size = shared_subscriber_queue_size_;
            opts.poll_timeout_ms = shared_hub_poll_timeout_ms_;
            opts.overflow_policy = OverflowPolicy::kDrop;
            opts.ring_mode = RingMode::SPSC;
            opts.coordinated_drop = false;
            std::string source_ref;
            for (size_t i = 0; i < plan->resolved_sources.size(); ++i) {
                if (i != 0) source_ref.push_back(',');
                source_ref += plan->resolved_sources[i];
            }
            hub = std::make_shared<SharedSourceHub>(
                plan->shared_hub_key,
                SharedHubMode::kDynamic,
                source_ref,
                plan->source_keys,
                plan->source,
                opts);
            shared_hubs_[plan->shared_hub_key] = hub;
            created = true;
        }
    }

    std::string bind_err;
    const int bind_rc = hub->BindWhereSignature(plan->where_signature, &bind_err);
    if (bind_rc != 0) {
        if (created) {
            std::lock_guard<std::mutex> lock(shared_hubs_mu_);
            auto it = shared_hubs_.find(plan->shared_hub_key);
            if (it != shared_hubs_.end() && it->second == hub) {
                shared_hubs_.erase(it);
            }
        }
        const ErrorCodeId code = bind_rc == EINVAL
            ? ErrorCodeId::kSharedSourceWhereMismatch
            : ErrorCodeId::kSharedSourceFilterUnsupported;
        *err_rsp = BuildExecutionErrorJson(
            bind_err.empty() ? "shared source WHERE binding failed" : bind_err,
            code,
            ErrorStageId::kSourceResolve);
        return bind_rc == EINVAL ? error::CONFLICT : error::BAD_REQUEST;
    }

    SharedSubscriberHandle handle;
    std::string sub_err;
    const int sub_rc = hub->AddSubscriber(
        plan->runtime_task_id,
        "",
        true,
        &handle,
        &sub_err,
        max_subscribers_per_hub_);
    if (sub_rc != 0 || !handle.Valid()) {
        if (created) {
            hub->RequestStop();
            hub->Join();
            std::lock_guard<std::mutex> lock(shared_hubs_mu_);
            auto it = shared_hubs_.find(plan->shared_hub_key);
            if (it != shared_hubs_.end() && it->second == hub) {
                shared_hubs_.erase(it);
            }
        }
        *err_rsp = BuildExecutionErrorJson(
            sub_err.empty() ? "shared source subscribe failed" : sub_err,
            ErrorCodeId::kSharedSourceSubscribeFailed,
            ErrorStageId::kSourceResolve);
        return MapStreamManagerErrorToStatus(sub_rc == 0 ? EIO : sub_rc);
    }
    if (!handle.Input()) {
        handle.Release();
        *err_rsp = BuildExecutionErrorJson(
            "shared source subscribe returned null input",
            ErrorCodeId::kSharedSourceSubscribeFailed,
            ErrorStageId::kSourceResolve);
        return error::INTERNAL_ERROR;
    }
    std::shared_ptr<IStreamChannel> attached_input = handle.Input();

    RuntimeSharedSubscription sub;
    sub.hub_key = plan->shared_hub_key;
    sub.handle = std::move(handle);
    {
        std::lock_guard<std::mutex> lock(runtime_subscriptions_mu_);
        runtime_subscriptions_[plan->runtime_task_id].push_back(std::move(sub));
    }
    *source_override = attached_input;
    if (!*source_override) {
        *err_rsp = BuildExecutionErrorJson(
            "shared source runtime input is null",
            ErrorCodeId::kSharedSourceInternalError,
            ErrorStageId::kSourceResolve);
        return error::INTERNAL_ERROR;
    }
    return error::OK;
}

}  // namespace scheduler
}  // namespace flowsql
