/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "scheduler_plugin.h"

#include <algorithm>
#include <chrono>
#include <unordered_set>

#include "scheduler_json_codec.h"

namespace flowsql {
namespace scheduler {

namespace {

int64_t CurrentTimeMsLocal() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

void SchedulerPlugin::ReleaseStreamTaskLeases(const std::string& runtime_task_id) {
    if (runtime_task_id.empty()) return;
    ReleaseRuntimeSubscriptions(runtime_task_id);
    std::lock_guard<std::mutex> lock(stream_channel_refs_mu_);
    auto it = stream_task_leases_.find(runtime_task_id);
    if (it == stream_task_leases_.end()) return;

    for (const auto& key : it->second.all_keys) {
        auto cnt_it = stream_channel_ref_counts_.find(key);
        if (cnt_it == stream_channel_ref_counts_.end()) continue;
        if (cnt_it->second <= 1) {
            stream_channel_ref_counts_.erase(cnt_it);
        } else {
            --cnt_it->second;
        }
    }
    for (const auto& key : it->second.source_keys) {
        auto lease_it = stream_source_leases_.find(key);
        if (lease_it != stream_source_leases_.end() &&
            lease_it->second.owner_id == it->second.lease_owner_id) {
            if (lease_it->second.ref_count <= 1) {
                stream_source_leases_.erase(lease_it);
            } else {
                --lease_it->second.ref_count;
            }
        }
    }
    stream_task_leases_.erase(it);
}

void SchedulerPlugin::ReleaseRuntimeSubscriptions(const std::string& runtime_task_id) {
    if (runtime_task_id.empty()) return;
    std::vector<RuntimeSharedSubscription> subs;
    {
        std::lock_guard<std::mutex> lock(runtime_subscriptions_mu_);
        auto it = runtime_subscriptions_.find(runtime_task_id);
        if (it != runtime_subscriptions_.end()) {
            subs = std::move(it->second);
            runtime_subscriptions_.erase(it);
        }
    }
    for (auto& sub : subs) {
        sub.handle.Release();
    }
    if (!subs.empty()) {
        PruneSharedHubs(false);
    }
}

void SchedulerPlugin::PruneSharedHubs(bool force_stop) {
    std::vector<std::pair<std::string, std::shared_ptr<SharedSourceHub>>> candidates;
    {
        std::lock_guard<std::mutex> lock(shared_hubs_mu_);
        candidates.reserve(shared_hubs_.size());
        for (const auto& kv : shared_hubs_) {
            if (!kv.second) continue;
            if (force_stop || kv.second->SubscriberCount() == 0) {
                candidates.push_back(kv);
            }
        }
    }
    for (const auto& entry : candidates) {
        if (!entry.second) continue;
        entry.second->RequestStop(force_stop);
        entry.second->Join();
    }
    if (candidates.empty()) return;
    std::lock_guard<std::mutex> lock(shared_hubs_mu_);
    for (const auto& entry : candidates) {
        auto it = shared_hubs_.find(entry.first);
        if (it == shared_hubs_.end()) continue;
        if (it->second != entry.second) continue;
        if (force_stop || (it->second->SubscriberCount() == 0 && it->second->IsTerminal())) {
            shared_hubs_.erase(it);
        }
    }
}

void SchedulerPlugin::SweepFinishedTaskLeases() {
    std::vector<std::string> to_release;
    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        for (const auto& [task_id, task] : stream_tasks_) {
            if (!task) {
                to_release.push_back(task_id);
                continue;
            }
            if (IsTerminalStreamTaskStatus(task->Status())) {
                to_release.push_back(task_id);
            }
        }
    }
    for (const auto& task_id : to_release) {
        MarkRuntimeTerminal(task_id, "single");
        ReleaseStreamTaskLeases(task_id);
    }
    if (!to_release.empty()) {
        std::lock_guard<std::mutex> lock(stream_group_nodes_mu_);
        for (const auto& task_id : to_release) {
            stream_group_node_owners_.erase(task_id);
        }
    }
}

void SchedulerPlugin::MarkRuntimeTerminal(const std::string& runtime_task_id,
                                          const std::string& runtime_kind,
                                          int64_t terminal_ms) {
    if (runtime_task_id.empty()) return;
    const int64_t now_ms = terminal_ms > 0 ? terminal_ms : CurrentTimeMsLocal();
    std::lock_guard<std::mutex> lock(stream_runtime_retention_mu_);
    stream_runtime_terminal_ms_[runtime_task_id] = now_ms;
    stream_runtime_last_access_ms_[runtime_task_id] = now_ms;
    stream_runtime_kind_[runtime_task_id] = runtime_kind.empty() ? "single" : runtime_kind;
}

void SchedulerPlugin::TouchRuntimeAccess(const std::string& runtime_task_id, int64_t now_ms) {
    if (runtime_task_id.empty()) return;
    const int64_t ts = now_ms > 0 ? now_ms : CurrentTimeMsLocal();
    std::lock_guard<std::mutex> lock(stream_runtime_retention_mu_);
    stream_runtime_last_access_ms_[runtime_task_id] = ts;
}

void SchedulerPlugin::SweepRuntimeRetainedObjects(int64_t now_ms) {
    // 逻辑链：
    // 1) 快照所有 terminal runtime 的终止时间、最近访问时间与类型；
    // 2) 按保留时长 + 最大保留数量计算淘汰集合；
    // 3) 按 single/group 分支释放 runtime 资源并清理关联索引；
    // 4) 最后统一回收 retention map 中的元数据键。
    struct RuntimeEntry {
        std::string runtime_task_id;
        std::string runtime_kind;
        int64_t terminal_ms = 0;
        int64_t last_access_ms = 0;
    };

    const int64_t now = now_ms > 0 ? now_ms : CurrentTimeMsLocal();
    batch_runtime_.SweepFinished(now, stream_runtime_retention_s_, stream_runtime_max_count_);
    std::vector<RuntimeEntry> terminal_entries;
    {
        std::lock_guard<std::mutex> lock(stream_runtime_retention_mu_);
        terminal_entries.reserve(stream_runtime_terminal_ms_.size());
        for (const auto& kv : stream_runtime_terminal_ms_) {
            RuntimeEntry entry;
            entry.runtime_task_id = kv.first;
            entry.terminal_ms = kv.second;
            auto it_access = stream_runtime_last_access_ms_.find(kv.first);
            entry.last_access_ms = (it_access == stream_runtime_last_access_ms_.end())
                                       ? kv.second
                                       : it_access->second;
            auto it_kind = stream_runtime_kind_.find(kv.first);
            entry.runtime_kind = (it_kind == stream_runtime_kind_.end()) ? "single" : it_kind->second;
            terminal_entries.push_back(std::move(entry));
        }
    }
    if (terminal_entries.empty()) return;

    std::unordered_set<std::string> remove_ids;
    const int64_t retention_ms = static_cast<int64_t>(stream_runtime_retention_s_) * 1000;
    if (stream_runtime_retention_s_ == 0) {
        for (const auto& entry : terminal_entries) {
            remove_ids.insert(entry.runtime_task_id);
        }
    } else {
        for (const auto& entry : terminal_entries) {
            if (now - entry.terminal_ms >= retention_ms) {
                remove_ids.insert(entry.runtime_task_id);
            }
        }
    }

    std::vector<RuntimeEntry> keep_candidates;
    keep_candidates.reserve(terminal_entries.size());
    for (const auto& entry : terminal_entries) {
        if (remove_ids.count(entry.runtime_task_id) != 0) continue;
        keep_candidates.push_back(entry);
    }
    if (keep_candidates.size() > stream_runtime_max_count_) {
        std::sort(keep_candidates.begin(), keep_candidates.end(),
                  [](const RuntimeEntry& a, const RuntimeEntry& b) {
                      if (a.last_access_ms != b.last_access_ms) {
                          return a.last_access_ms < b.last_access_ms;
                      }
                      if (a.terminal_ms != b.terminal_ms) {
                          return a.terminal_ms < b.terminal_ms;
                      }
                      return a.runtime_task_id < b.runtime_task_id;
                  });
        const size_t over = keep_candidates.size() - stream_runtime_max_count_;
        for (size_t i = 0; i < over; ++i) {
            remove_ids.insert(keep_candidates[i].runtime_task_id);
        }
    }
    if (remove_ids.empty()) return;

    std::vector<std::string> removed_ids;
    removed_ids.reserve(remove_ids.size());
    for (const auto& entry : terminal_entries) {
        if (remove_ids.count(entry.runtime_task_id) == 0) continue;

        const bool is_group = (entry.runtime_kind == "group");
        if (is_group) {
            std::shared_ptr<StreamTaskGroup> group;
            {
                std::lock_guard<std::mutex> lock(stream_task_groups_mu_);
                auto it = stream_task_groups_.find(entry.runtime_task_id);
                if (it != stream_task_groups_.end()) {
                    group = it->second;
                    stream_task_groups_.erase(it);
                }
            }
            StreamGroupSnapshot snapshot;
            StreamGroupSnapshot* snapshot_ptr = nullptr;
            if (group) {
                snapshot = group->Snapshot();
                if (!IsTerminalStreamGroupStatus(snapshot.status)) {
                    std::lock_guard<std::mutex> lock(stream_task_groups_mu_);
                    stream_task_groups_[entry.runtime_task_id] = group;
                    continue;
                }
                snapshot_ptr = &snapshot;
            }
            CleanupGroupRuntimeResources(entry.runtime_task_id, snapshot_ptr);
            if (snapshot_ptr) {
                std::lock_guard<std::mutex> lock(stream_tasks_mu_);
                for (const auto& node : snapshot.nodes) {
                    stream_tasks_.erase(node.runtime_task_id);
                }
            }
            {
                std::lock_guard<std::mutex> lock(stream_group_node_sources_mu_);
                stream_group_node_sources_.erase(entry.runtime_task_id);
            }
            {
                std::lock_guard<std::mutex> lock(stream_group_share_set_snapshots_mu_);
                stream_group_share_set_snapshots_.erase(entry.runtime_task_id);
            }
        } else {
            std::shared_ptr<StreamTask> task;
            {
                std::lock_guard<std::mutex> lock(stream_tasks_mu_);
                auto it = stream_tasks_.find(entry.runtime_task_id);
                if (it != stream_tasks_.end()) {
                    task = it->second;
                }
                if (task && !IsTerminalStreamTaskStatus(task->Status())) {
                    continue;
                }
                if (it != stream_tasks_.end()) {
                    stream_tasks_.erase(it);
                }
            }
            ReleaseStreamTaskLeases(entry.runtime_task_id);
            {
                std::lock_guard<std::mutex> lock(stream_group_nodes_mu_);
                stream_group_node_owners_.erase(entry.runtime_task_id);
            }
        }
        removed_ids.push_back(entry.runtime_task_id);
    }

    if (removed_ids.empty()) return;
    std::lock_guard<std::mutex> lock(stream_runtime_retention_mu_);
    for (const auto& id : removed_ids) {
        stream_runtime_terminal_ms_.erase(id);
        stream_runtime_last_access_ms_.erase(id);
        stream_runtime_kind_.erase(id);
    }
}

int SchedulerPlugin::QueryStreamTaskSnapshotByRuntimeId(const std::string& runtime_task_id,
                                                        TaskSnapshot* snapshot_out) {
    if (!snapshot_out || runtime_task_id.empty()) return EINVAL;
    std::shared_ptr<StreamTask> task;
    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        auto it = stream_tasks_.find(runtime_task_id);
        if (it == stream_tasks_.end() || !it->second) return ENOENT;
        task = it->second;
    }
    *snapshot_out = task->Snapshot();
    return 0;
}

int SchedulerPlugin::QueryRuntimeSharedHubSnapshot(const std::string& runtime_task_id,
                                                   SharedHubSnapshot* snapshot_out) {
    if (!snapshot_out || runtime_task_id.empty()) return EINVAL;
    snapshot_out->id.clear();

    std::string hub_key;
    {
        std::lock_guard<std::mutex> lock(runtime_subscriptions_mu_);
        auto it = runtime_subscriptions_.find(runtime_task_id);
        if (it == runtime_subscriptions_.end() || it->second.empty()) {
            return ENOENT;
        }
        hub_key = it->second.front().hub_key;
    }
    if (hub_key.empty()) return ENOENT;

    std::shared_ptr<SharedSourceHub> hub;
    {
        std::lock_guard<std::mutex> lock(shared_hubs_mu_);
        auto it = shared_hubs_.find(hub_key);
        if (it == shared_hubs_.end() || !it->second) {
            return ENOENT;
        }
        hub = it->second;
    }
    *snapshot_out = hub->Snapshot();
    return 0;
}

void SchedulerPlugin::RequestStopStreamTaskByRuntimeId(const std::string& runtime_task_id) {
    if (runtime_task_id.empty()) return;
    std::shared_ptr<StreamTask> task;
    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        auto it = stream_tasks_.find(runtime_task_id);
        if (it == stream_tasks_.end() || !it->second) return;
        task = it->second;
    }
    task->RequestStop();
}

std::vector<SharedHubSnapshot> SchedulerPlugin::QueryGroupShareSetSnapshots(
    const std::string& group_runtime_task_id) {
    std::vector<SharedHubSnapshot> out;
    {
        std::lock_guard<std::mutex> lock(stream_group_share_set_snapshots_mu_);
        auto it = stream_group_share_set_snapshots_.find(group_runtime_task_id);
        if (it != stream_group_share_set_snapshots_.end()) {
            return it->second;
        }
    }
    std::vector<StreamGroupShareSetRuntime> runtimes;
    {
        std::lock_guard<std::mutex> lock(stream_group_share_sets_mu_);
        auto it = stream_group_share_sets_.find(group_runtime_task_id);
        if (it == stream_group_share_sets_.end()) {
            return out;
        }
        runtimes = it->second;
    }
    out.reserve(runtimes.size());
    for (const auto& runtime : runtimes) {
        SharedHubSnapshot snap;
        if (runtime.hub) {
            snap = runtime.hub->Snapshot();
        }
        if (snap.id.empty()) {
            snap.id = runtime.id;
            snap.source_ref = runtime.source_ref;
            snap.members = runtime.members;
        }
        out.push_back(std::move(snap));
    }
    return out;
}

std::unordered_map<std::string, GroupNodeResolvedSourceMeta> SchedulerPlugin::QueryGroupNodeResolvedSources(
    const std::string& group_runtime_task_id) {
    std::unordered_map<std::string, GroupNodeResolvedSourceMeta> out;
    if (group_runtime_task_id.empty()) return out;
    std::lock_guard<std::mutex> lock(stream_group_node_sources_mu_);
    auto it = stream_group_node_sources_.find(group_runtime_task_id);
    if (it == stream_group_node_sources_.end()) {
        return out;
    }
    out = it->second;
    return out;
}

void SchedulerPlugin::CleanupGroupRuntimeResources(const std::string& group_runtime_task_id,
                                                   const StreamGroupSnapshot* group_snapshot) {
    if (group_runtime_task_id.empty()) return;

    std::vector<std::string> batch_node_runtime_ids;
    {
        std::lock_guard<std::mutex> lock(stream_group_batch_nodes_mu_);
        auto it = stream_group_batch_node_runtime_ids_.find(group_runtime_task_id);
        if (it != stream_group_batch_node_runtime_ids_.end()) {
            batch_node_runtime_ids = std::move(it->second);
            stream_group_batch_node_runtime_ids_.erase(it);
        }
    }
    if (group_snapshot) {
        for (const auto& node : group_snapshot->nodes) {
            if (node.kind != GroupNodeKind::kBatch || node.runtime_task_id.empty()) continue;
            batch_node_runtime_ids.push_back(node.runtime_task_id);
        }
    }
    if (!batch_node_runtime_ids.empty()) {
        std::sort(batch_node_runtime_ids.begin(), batch_node_runtime_ids.end());
        batch_node_runtime_ids.erase(
            std::unique(batch_node_runtime_ids.begin(), batch_node_runtime_ids.end()),
            batch_node_runtime_ids.end());
        for (const auto& node_runtime_id : batch_node_runtime_ids) {
            std::string stop_err;
            batch_runtime_.RequestStop(node_runtime_id, &stop_err);
        }
    }

    std::vector<StreamGroupShareSetRuntime> share_sets;
    {
        std::lock_guard<std::mutex> lock(stream_group_share_sets_mu_);
        auto it = stream_group_share_sets_.find(group_runtime_task_id);
        if (it != stream_group_share_sets_.end()) {
            share_sets = std::move(it->second);
            stream_group_share_sets_.erase(it);
        }
    }

    std::vector<SharedHubSnapshot> final_snapshots;
    final_snapshots.reserve(share_sets.size());
    for (auto& ss : share_sets) {
        if (ss.hub) {
            final_snapshots.push_back(ss.hub->Snapshot());
            ss.hub->RequestStop();
            ss.hub->Join();
            final_snapshots.back() = ss.hub->Snapshot();
        } else {
            SharedHubSnapshot snap;
            snap.id = ss.id;
            snap.source_ref = ss.source_ref;
            snap.members = ss.members;
            final_snapshots.push_back(std::move(snap));
        }
        for (const auto& channel_ref : ss.internal_channel_refs) {
            EraseManagedChannel(channel_ref);
        }
    }
    if (!final_snapshots.empty()) {
        std::lock_guard<std::mutex> lock(stream_group_share_set_snapshots_mu_);
        stream_group_share_set_snapshots_[group_runtime_task_id] = std::move(final_snapshots);
    }

    {
        std::lock_guard<std::mutex> lock(stream_group_nodes_mu_);
        for (auto it = stream_group_node_owners_.begin(); it != stream_group_node_owners_.end();) {
            if (it->second == group_runtime_task_id) {
                it = stream_group_node_owners_.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (group_snapshot) {
        for (const auto& node : group_snapshot->nodes) {
            ReleaseStreamTaskLeases(node.runtime_task_id);
        }
    }
    ReleaseStreamTaskLeases(group_runtime_task_id);
}

}  // namespace scheduler
}  // namespace flowsql
