#include "scheduler_plugin.h"

#include <unordered_set>

#include "framework/interfaces/ichannel_registry.h"
#include "framework/interfaces/idatabase_factory.h"
#include "framework/interfaces/idataframe_channel.h"
#include "framework/interfaces/istream_factory.h"
#include "scheduler_internal_utils.h"

namespace flowsql {
namespace scheduler {

namespace {

std::shared_ptr<IChannel> MakeNonOwningChannelHolderLocal(IChannel* ch) {
    if (!ch) return nullptr;
    return std::shared_ptr<IChannel>(ch, [](IChannel*) {});
}

}  // namespace

void SchedulerPlugin::RegisterManagedChannel(const std::string& key, std::shared_ptr<IChannel> ch) {
    if (key.empty() || !ch) return;
    std::lock_guard<std::mutex> lock(channels_mu_);
    channels_[key] = std::move(ch);
}

void SchedulerPlugin::EraseManagedChannel(const std::string& key) {
    if (key.empty()) return;
    std::lock_guard<std::mutex> lock(channels_mu_);
    channels_.erase(key);
}

void SchedulerPlugin::ClearManagedChannels() {
    std::lock_guard<std::mutex> lock(channels_mu_);
    channels_.clear();
}

std::shared_ptr<IChannel> SchedulerPlugin::FindManagedChannelShared(const std::string& key) {
    std::lock_guard<std::mutex> lock(channels_mu_);
    auto it = channels_.find(key);
    if (it == channels_.end()) return nullptr;
    return it->second;
}

std::vector<std::pair<std::string, std::shared_ptr<IChannel>>> SchedulerPlugin::SnapshotManagedChannels() {
    std::vector<std::pair<std::string, std::shared_ptr<IChannel>>> snapshot;
    std::lock_guard<std::mutex> lock(channels_mu_);
    snapshot.reserve(channels_.size());
    for (const auto& kv : channels_) {
        snapshot.push_back(kv);
    }
    return snapshot;
}

void SchedulerPlugin::RegisterChannel(const std::string& key, std::shared_ptr<IChannel> ch) {
    RegisterManagedChannel(key, std::move(ch));
}

IChannel* SchedulerPlugin::FindChannel(const std::string& name) {
    return FindChannel(name, nullptr);
}

IChannel* SchedulerPlugin::FindChannel(const std::string& name, std::shared_ptr<IChannel>* owner_out) {
    if (owner_out) owner_out->reset();
    auto* ch_registry = querier_ ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY)) : nullptr;
    if (IsDataframeRefName(name) && ch_registry) {
        auto ch = ch_registry->Get(DataframeNamePart(name).c_str());
        auto* df = dynamic_cast<IDataFrameChannel*>(ch.get());
        if (df) {
            if (owner_out) *owner_out = std::move(ch);
            return df;
        }
    }

    if (auto managed = FindManagedChannelShared(name)) {
        if (owner_out) *owner_out = managed;
        return managed.get();
    }

    if (IsStreamRefName(name) && querier_) {
        auto* stream_factory = static_cast<IStreamFactory*>(querier_->First(IID_STREAM_FACTORY));
        if (stream_factory) {
            const std::string target = StreamNamePart(name);
            IStreamChannel* matched = nullptr;
            bool ambiguous = false;
            stream_factory->List([&](const char*, const char* stream_name, IStreamChannel* stream_ch) {
                if (!stream_name || !stream_ch) return;
                if (target != stream_name) return;
                if (matched && matched != stream_ch) {
                    ambiguous = true;
                    return;
                }
                matched = stream_ch;
            });
            if (!ambiguous && matched) {
                if (owner_out) *owner_out = MakeNonOwningChannelHolderLocal(matched);
                return matched;
            }
        }
    }

    IChannel* found = nullptr;
    if (querier_) {
        querier_->Traverse(IID_CHANNEL, [&](void* p) -> int {
            auto* c = static_cast<IChannel*>(p);
            auto dot = name.find('.');
            bool category_and_name_match = false;
            if (dot != std::string::npos) {
                const std::string req_category = name.substr(0, dot);
                const std::string req_name = name.substr(dot + 1);
                category_and_name_match = IEquals(c->Category(), req_category) && std::string(c->Name()) == req_name;
            }
            if (category_and_name_match || std::string(c->Name()) == name) {
                found = c;
                return -1;
            }
            return 0;
        });
    }
    if (found && owner_out) *owner_out = MakeNonOwningChannelHolderLocal(found);

    if (!found) {
        if (querier_) {
            auto* factory = static_cast<IDatabaseFactory*>(
                querier_->First(IID_DATABASE_FACTORY));
            if (factory) {
                auto pos = name.find('.');
                if (pos != std::string::npos) {
                    std::string type = ToLowerAscii(name.substr(0, pos));
                    std::string rest = name.substr(pos + 1);
                    auto pos2 = rest.find('.');
                    std::string db_name = (pos2 != std::string::npos) ? rest.substr(0, pos2) : rest;

                    auto* db_ch = factory->Get(type.c_str(), db_name.c_str());
                    if (db_ch) found = db_ch;
                }
            }
        }
    }
    if (found && owner_out && !*owner_out) *owner_out = MakeNonOwningChannelHolderLocal(found);

    if (!found && querier_) {
        auto* stream_factory = static_cast<IStreamFactory*>(querier_->First(IID_STREAM_FACTORY));
        if (stream_factory) {
            auto pos = name.find('.');
            if (pos != std::string::npos) {
                const std::string type = ToLowerAscii(name.substr(0, pos));
                const std::string rest = name.substr(pos + 1);
                const auto pos2 = rest.find('.');
                const std::string stream_name = (pos2 != std::string::npos) ? rest.substr(0, pos2) : rest;
                auto* stream_ch = stream_factory->Get(type.c_str(), stream_name.c_str());
                if (stream_ch) found = stream_ch;
            }
        }
    }
    if (found && owner_out && !*owner_out) *owner_out = MakeNonOwningChannelHolderLocal(found);

    return found;
}

int SchedulerPlugin::TryAcquireStreamTaskLeases(const std::string& runtime_task_id,
                                                const std::vector<std::string>& source_keys,
                                                const std::vector<std::string>& sink_keys,
                                                std::string* conflict_key_out,
                                                bool* blocked_by_mutation_out,
                                                const std::string& lease_owner_id,
                                                const std::unordered_map<std::string, uint64_t>* expected_versions,
                                                std::string* version_conflict_key_out) {
    if (runtime_task_id.empty()) return EINVAL;
    if (blocked_by_mutation_out) *blocked_by_mutation_out = false;
    if (version_conflict_key_out) version_conflict_key_out->clear();
    const std::string owner_id = lease_owner_id.empty() ? runtime_task_id : lease_owner_id;
    std::lock_guard<std::mutex> lock(stream_channel_refs_mu_);

    std::unordered_set<std::string> unique_all;
    for (const auto& key : source_keys) unique_all.insert(key);
    for (const auto& key : sink_keys) unique_all.insert(key);

    if (expected_versions) {
        for (const auto& key : unique_all) {
            const auto expected_it = expected_versions->find(key);
            const uint64_t expected = (expected_it == expected_versions->end()) ? 0 : expected_it->second;
            const auto version_it = stream_channel_versions_.find(key);
            const uint64_t current = (version_it == stream_channel_versions_.end()) ? 0 : version_it->second;
            if (current != expected) {
                if (conflict_key_out) *conflict_key_out = key;
                if (version_conflict_key_out) *version_conflict_key_out = key;
                return EAGAIN;
            }
        }
    }

    for (const auto& key : unique_all) {
        if (stream_channel_mutating_.count(key) > 0) {
            if (conflict_key_out) *conflict_key_out = key;
            if (blocked_by_mutation_out) *blocked_by_mutation_out = true;
            return EBUSY;
        }
    }

    for (const auto& key : source_keys) {
        auto lease_it = stream_source_leases_.find(key);
        if (lease_it != stream_source_leases_.end() &&
            lease_it->second.owner_id != owner_id) {
            if (conflict_key_out) *conflict_key_out = key;
            return EBUSY;
        }
    }

    StreamTaskLeaseInfo info;
    info.all_keys.assign(unique_all.begin(), unique_all.end());
    info.source_keys = source_keys;
    info.lease_owner_id = owner_id;

    for (const auto& key : info.all_keys) {
        stream_channel_ref_counts_[key] += 1;
    }
    for (const auto& key : source_keys) {
        auto& state = stream_source_leases_[key];
        if (state.owner_id.empty()) {
            state.owner_id = owner_id;
        }
        state.ref_count += 1;
    }
    stream_task_leases_[runtime_task_id] = std::move(info);
    return 0;
}

void SchedulerPlugin::CaptureStreamChannelVersionSnapshot(
    const std::vector<std::string>& keys,
    std::unordered_map<std::string, uint64_t>* snapshot_out) {
    if (!snapshot_out) return;
    snapshot_out->clear();
    std::lock_guard<std::mutex> lock(stream_channel_refs_mu_);
    for (const auto& key : keys) {
        if (key.empty()) continue;
        auto it = stream_channel_versions_.find(key);
        snapshot_out->emplace(key, it == stream_channel_versions_.end() ? 0 : it->second);
    }
}

int SchedulerPlugin::TryBeginStreamChannelMutation(const std::string& key, std::string* reason_out) {
    if (reason_out) reason_out->clear();
    if (key.empty()) return EINVAL;

    std::lock_guard<std::mutex> lock(stream_channel_refs_mu_);
    auto it = stream_channel_ref_counts_.find(key);
    if (it != stream_channel_ref_counts_.end() && it->second > 0) {
        if (reason_out) *reason_out = "in_use";
        return EBUSY;
    }
    if (stream_source_leases_.find(key) != stream_source_leases_.end()) {
        if (reason_out) *reason_out = "source_in_use";
        return EBUSY;
    }
    if (stream_channel_mutating_.count(key) > 0) {
        if (reason_out) *reason_out = "mutating";
        return EBUSY;
    }
    stream_channel_versions_[key] += 1;
    stream_channel_mutating_.insert(key);
    return 0;
}

void SchedulerPlugin::EndStreamChannelMutation(const std::string& key) {
    if (key.empty()) return;
    std::lock_guard<std::mutex> lock(stream_channel_refs_mu_);
    stream_channel_mutating_.erase(key);
}

}  // namespace scheduler
}  // namespace flowsql
