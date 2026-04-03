#include "stream_plugin.h"

#include <framework/builtin/stream/tcp_session_mock_stream_channel.h>
#include <framework/core/ring_stream_channel.h>

#include <common/log.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <filesystem>
#include <tuple>
#include <vector>
#include <sqlite3.h>
#include <yaml-cpp/yaml.h>

namespace flowsql {
namespace stream {

thread_local std::string StreamPlugin::last_error_;

namespace {

static const char* kStreamStoreSchemaSql =
    "CREATE TABLE IF NOT EXISTS stream_channel_store ("
    "type TEXT NOT NULL,"
    "name TEXT NOT NULL,"
    "option TEXT NOT NULL DEFAULT '',"
    "status TEXT NOT NULL DEFAULT 'active',"
    "updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "PRIMARY KEY(type, name)"
    ");";

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool ParseBool(const std::string& value, bool* out) {
    if (!out) return false;
    const std::string v = ToLowerAscii(value);
    if (v == "1" || v == "true" || v == "yes" || v == "on") {
        *out = true;
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off") {
        *out = false;
        return true;
    }
    return false;
}

int ParseRingOptions(const std::string& option, RingStreamChannelOptions* out) {
    if (!out) return EINVAL;
    if (option.empty()) return 0;

    size_t pos = 0;
    while (pos < option.size()) {
        const size_t eq = option.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = option.find(';', eq + 1);
        if (end == std::string::npos) end = option.size();

        const std::string key = option.substr(pos, eq - pos);
        const std::string value = option.substr(eq + 1, end - eq - 1);
        try {
            if (key == "ring_size") {
                out->ring_size = static_cast<size_t>(std::stoull(value));
            } else if (key == "batch_rows") {
                out->batch_rows = std::stoi(value);
            } else if (key == "overflow") {
                out->overflow = ParseOverflowPolicy(value);
            } else if (key == "ring_mode") {
                out->ring_mode = ParseRingMode(value);
            } else if (key == "finite") {
                bool parsed = false;
                if (!ParseBool(value, &parsed)) return EINVAL;
                out->finite = parsed;
            }
        } catch (...) {
            return EINVAL;
        }

        pos = (end < option.size()) ? end + 1 : option.size();
    }
    return 0;
}

int ParseTcpSessionMockOptions(const std::string& option, TcpSessionMockOptions* out) {
    if (!out) return EINVAL;
    out->mode = TcpSessionMockMode::kNone;
    out->total_records = 1024;
    out->batch_rows = 64;
    out->emit_interval_ms = 0;
    out->partition_count = 4;
    out->queue_options.ring_size = 256;
    out->queue_options.batch_rows = 1024;
    out->queue_options.overflow = OverflowPolicy::kDrop;
    out->queue_options.ring_mode = RingMode::SPSC;
    out->queue_options.finite = true;
    if (option.empty()) return 0;

    size_t pos = 0;
    while (pos < option.size()) {
        const size_t eq = option.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = option.find(';', eq + 1);
        if (end == std::string::npos) end = option.size();

        const std::string key = option.substr(pos, eq - pos);
        const std::string value = option.substr(eq + 1, end - eq - 1);
        try {
            if (key == "mode") {
                out->mode = ParseTcpSessionMockMode(value);
            } else if (key == "total_records") {
                out->total_records = std::stoll(value);
            } else if (key == "batch_rows") {
                out->batch_rows = std::stoi(value);
            } else if (key == "emit_interval_ms") {
                out->emit_interval_ms = std::stoi(value);
            } else if (key == "partition_count") {
                out->partition_count = std::stoi(value);
            } else if (key == "ring_size") {
                out->queue_options.ring_size = static_cast<size_t>(std::stoull(value));
            } else if (key == "overflow") {
                out->queue_options.overflow = ParseOverflowPolicy(value);
            } else if (key == "ring_mode") {
                out->queue_options.ring_mode = ParseRingMode(value);
            }
        } catch (...) {
            return EINVAL;
        }

        pos = (end < option.size()) ? end + 1 : option.size();
    }
    return 0;
}

}  // namespace

int StreamPlugin::Option(const char* arg) {
    if (!arg || !*arg) return 0;

    std::string opts(arg);
    size_t pos = 0;
    while (pos < opts.size()) {
        const size_t eq = opts.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = opts.find(';', eq + 1);
        if (end == std::string::npos) end = opts.size();

        const std::string key = opts.substr(pos, eq - pos);
        const std::string value = opts.substr(eq + 1, end - eq - 1);
        if (key == "config_file") {
            config_file_ = value;
        } else if (key == "db_path") {
            db_path_ = value;
        }

        pos = (end < opts.size()) ? end + 1 : opts.size();
    }
    return 0;
}

int StreamPlugin::Load(IQuerier* querier) {
    std::lock_guard<std::mutex> lock(mutex_);
    querier_ = querier;

    configs_.clear();
    channels_.clear();

    if (StoreEnabled()) {
        int db_rc = EnsureStoreReadyLocked();
        if (db_rc != 0) return db_rc;
    }

    int rc = 0;
    if (StoreEnabled()) {
        rc = LoadFromStoreLocked();
        if (rc != 0) return rc;
    }

    if (configs_.empty() && !config_file_.empty()) {
        rc = LoadFromYamlLocked();
        if (rc != 0) return rc;
        if (StoreEnabled()) {
            rc = PersistAllIfStoreEmptyLocked();
            if (rc != 0) return rc;
        }
    } else {
        rc = BuildChannelsLocked();
        if (rc != 0) return rc;
    }

    return 0;
}

int StreamPlugin::Unload() {
    Stop();
    std::lock_guard<std::mutex> lock(mutex_);
    channels_.clear();
    configs_.clear();
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    querier_ = nullptr;
    return 0;
}

int StreamPlugin::Start() {
    return 0;
}

int StreamPlugin::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [_, channel] : channels_) {
        if (channel) {
            channel->Close();
        }
    }
    // Keep channel objects alive until Unload().
    // Scheduler may still hold non-owning references during StopAll order.
    return 0;
}

IStreamChannel* StreamPlugin::Get(const char* type, const char* name) {
    if (!type || !name) return nullptr;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(MakeKey(type, name));
    if (it == channels_.end()) return nullptr;
    return it->second.get();
}

void StreamPlugin::List(std::function<void(const char* type, const char* name, IStreamChannel*)> callback) {
    if (!callback) return;

    std::vector<std::pair<StreamChannelConfig, std::shared_ptr<IStreamChannel>>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot.reserve(channels_.size());
        for (const auto& [key, channel] : channels_) {
            auto cfg_it = configs_.find(key);
            if (cfg_it == configs_.end()) continue;
            snapshot.push_back({cfg_it->second, channel});
        }
    }

    for (auto& entry : snapshot) {
        callback(entry.first.type.c_str(), entry.first.name.c_str(), entry.second.get());
    }
}

int StreamPlugin::LoadFromYamlLocked() {
    YAML::Node root;
    try {
        root = YAML::LoadFile(config_file_);
    } catch (const std::exception& e) {
        last_error_ = std::string("load stream config failed: ") + e.what();
        LOG_ERROR("StreamPlugin::LoadFromYamlLocked: %s", last_error_.c_str());
        return -1;
    }

    const YAML::Node channels = root["channels"];
    if (!channels || !channels["stream_channels"]) {
        return 0;
    }

    const YAML::Node stream_channels = channels["stream_channels"];
    if (!stream_channels.IsSequence()) {
        last_error_ = "channels.stream_channels must be sequence";
        return -1;
    }

    for (const auto& node : stream_channels) {
        if (!node.IsMap()) continue;
        if (!node["type"] || !node["name"]) continue;

        StreamChannelConfig cfg;
        cfg.type = ToLowerAscii(node["type"].as<std::string>());
        cfg.name = node["name"].as<std::string>();
        if (node["option"]) {
            cfg.option = node["option"].as<std::string>();
        }

        if (cfg.type.empty() || cfg.name.empty()) continue;
        configs_[MakeKey(cfg.type, cfg.name)] = std::move(cfg);
    }

    return BuildChannelsLocked();
}

int StreamPlugin::BuildChannelsLocked() {
    channels_.clear();
    for (const auto& [key, cfg] : configs_) {
        std::shared_ptr<IStreamChannel> channel;
        const int rc = BuildOneChannelLocked(cfg, &channel);
        if (rc != 0) {
            if (rc == ENOTSUP) {
                continue;
            }
            return rc;
        }
        channels_[key] = std::move(channel);
    }
    return 0;
}

int StreamPlugin::BuildOneChannelLocked(const StreamChannelConfig& cfg,
                                        std::shared_ptr<IStreamChannel>* out) {
    if (!out) return EINVAL;
    out->reset();

    if (cfg.type == "ring") {
        RingStreamChannelOptions options;
        const int parse_rc = ParseRingOptions(cfg.option, &options);
        if (parse_rc != 0) {
            last_error_ = "invalid ring option for stream channel: " + cfg.type + "." + cfg.name;
            return EINVAL;
        }

        auto channel = std::make_shared<RingStreamChannel>(cfg.type, cfg.name, options);
        const int open_rc = channel->Open();
        if (open_rc != 0) {
            last_error_ = "open stream channel failed: " + cfg.type + "." + cfg.name;
            return open_rc;
        }
        *out = std::move(channel);
        return 0;
    }

    if (cfg.type == "tcp_session_mock") {
        TcpSessionMockOptions options;
        const int parse_rc = ParseTcpSessionMockOptions(cfg.option, &options);
        if (parse_rc != 0) {
            last_error_ = "invalid tcp_session_mock option for stream channel: " + cfg.type + "." + cfg.name;
            return EINVAL;
        }

        auto channel = std::make_shared<TcpSessionMockStreamChannel>(cfg.type, cfg.name, options);
        const int open_rc = channel->Open();
        if (open_rc != 0) {
            last_error_ = "open stream channel failed: " + cfg.type + "." + cfg.name;
            return open_rc;
        }
        *out = std::move(channel);
        return 0;
    }

    // 路径 B 类型（如 netcard）本 Sprint 不实现，跳过加载。
    last_error_ = "unsupported stream channel type: " + cfg.type;
    LOG_WARN("StreamPlugin::BuildOneChannelLocked: unsupported stream type=%s name=%s",
             cfg.type.c_str(), cfg.name.c_str());
    return ENOTSUP;
}

int StreamPlugin::EnsureStoreReadyLocked() {
    if (!StoreEnabled()) return 0;
    if (db_) return 0;

    std::error_code ec;
    std::filesystem::path parent = std::filesystem::path(db_path_).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            last_error_ = "create stream db parent failed: " + ec.message();
            return -1;
        }
    }

    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(db_path_.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
        last_error_ = "open stream db failed";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        return -1;
    }
    (void)sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    if (sqlite3_exec(db_, kStreamStoreSchemaSql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        last_error_ = "init stream_channel_store schema failed";
        return -1;
    }
    return 0;
}

int StreamPlugin::UpsertStoreLocked(const StreamChannelConfig& cfg) {
    if (!StoreEnabled()) return ENOTSUP;
    if (!db_) return ENOTSUP;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO stream_channel_store(type, name, option, status, updated_at) "
        "VALUES(?1, ?2, ?3, 'active', CURRENT_TIMESTAMP) "
        "ON CONFLICT(type, name) DO UPDATE SET option=excluded.option, status='active', updated_at=CURRENT_TIMESTAMP;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, cfg.type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, cfg.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, cfg.option.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int StreamPlugin::DeleteStoreLocked(const std::string& type, const std::string& name) {
    if (!StoreEnabled()) return ENOTSUP;
    if (!db_) return ENOTSUP;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM stream_channel_store WHERE type=?1 AND name=?2;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int StreamPlugin::LoadFromStoreLocked() {
    if (!StoreEnabled()) return 0;
    if (!db_) return -1;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT type, name, option FROM stream_channel_store ORDER BY type ASC, name ASC;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* option = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        if (!type || !name) continue;
        StreamChannelConfig cfg;
        cfg.type = ToLowerAscii(type);
        cfg.name = name;
        cfg.option = option ? option : "";
        if (cfg.type.empty() || cfg.name.empty()) continue;
        configs_[MakeKey(cfg.type, cfg.name)] = std::move(cfg);
    }
    sqlite3_finalize(stmt);
    return BuildChannelsLocked();
}

int StreamPlugin::PersistAllIfStoreEmptyLocked() {
    if (!StoreEnabled()) return ENOTSUP;
    if (!db_) return ENOTSUP;

    sqlite3_stmt* cnt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(1) FROM stream_channel_store;", -1, &cnt, nullptr) != SQLITE_OK) {
        return -1;
    }
    int64_t rows = 0;
    if (sqlite3_step(cnt) == SQLITE_ROW) {
        rows = sqlite3_column_int64(cnt, 0);
    }
    sqlite3_finalize(cnt);
    if (rows > 0) return 0;

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, nullptr) != SQLITE_OK) return -1;
    bool ok = true;
    for (const auto& [_, cfg] : configs_) {
        if (UpsertStoreLocked(cfg) != 0) {
            ok = false;
            break;
        }
    }
    if (ok && sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK) {
        return 0;
    }
    (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return -1;
}

int StreamPlugin::AddChannel(const std::string& type,
                             const std::string& name,
                             const std::string& option) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!StoreEnabled()) {
        last_error_ = "stream manager requires db_path";
        return ENOTSUP;
    }
    if (EnsureStoreReadyLocked() != 0) return -1;
    if (type.empty() || name.empty()) return EINVAL;

    StreamChannelConfig cfg{ToLowerAscii(type), name, option};
    const std::string key = MakeKey(cfg.type, cfg.name);
    if (configs_.find(key) != configs_.end()) return EEXIST;

    std::shared_ptr<IStreamChannel> channel;
    const int build_rc = BuildOneChannelLocked(cfg, &channel);
    if (build_rc != 0) return build_rc;
    if (UpsertStoreLocked(cfg) != 0) {
        if (channel) channel->Close();
        return -1;
    }

    configs_[key] = cfg;
    channels_[key] = std::move(channel);
    return 0;
}

int StreamPlugin::ModifyChannel(const std::string& type,
                                const std::string& name,
                                const std::string& option) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!StoreEnabled()) {
        last_error_ = "stream manager requires db_path";
        return ENOTSUP;
    }
    if (EnsureStoreReadyLocked() != 0) return -1;
    if (type.empty() || name.empty()) return EINVAL;

    const std::string type_l = ToLowerAscii(type);
    const std::string key = MakeKey(type_l, name);
    auto cfg_it = configs_.find(key);
    if (cfg_it == configs_.end()) return ENOENT;

    StreamChannelConfig next_cfg{type_l, name, option};
    std::shared_ptr<IStreamChannel> next_channel;
    const int build_rc = BuildOneChannelLocked(next_cfg, &next_channel);
    if (build_rc != 0) return build_rc;

    if (UpsertStoreLocked(next_cfg) != 0) {
        if (next_channel) next_channel->Close();
        return -1;
    }

    auto old_channel = channels_[key];
    channels_[key] = std::move(next_channel);
    cfg_it->second = next_cfg;
    if (old_channel) {
        old_channel->Close();
    }
    return 0;
}

int StreamPlugin::RemoveChannel(const std::string& type,
                                const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!StoreEnabled()) {
        last_error_ = "stream manager requires db_path";
        return ENOTSUP;
    }
    if (EnsureStoreReadyLocked() != 0) return -1;
    if (type.empty() || name.empty()) return EINVAL;

    const std::string type_l = ToLowerAscii(type);
    const std::string key = MakeKey(type_l, name);
    auto cfg_it = configs_.find(key);
    if (cfg_it == configs_.end()) return ENOENT;

    if (DeleteStoreLocked(type_l, name) != 0) return -1;

    auto ch_it = channels_.find(key);
    if (ch_it != channels_.end()) {
        if (ch_it->second) ch_it->second->Close();
        channels_.erase(ch_it);
    }
    configs_.erase(cfg_it);
    return 0;
}

void StreamPlugin::QueryChannels(std::function<void(const std::string& type,
                                                    const std::string& name,
                                                    const std::string& option,
                                                    const std::string& status)> callback) {
    if (!callback) return;

    std::vector<std::tuple<std::string, std::string, std::string, std::string>> items;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items.reserve(configs_.size());
        for (const auto& [key, cfg] : configs_) {
            std::string status = "running";
            auto it = channels_.find(key);
            if (it == channels_.end() || !it->second) {
                status = "missing";
            } else if (it->second->IsFinished() && it->second->IsEmpty()) {
                status = "stopped";
            } else if (it->second->IsFinished()) {
                status = "draining";
            }
            items.emplace_back(cfg.type, cfg.name, cfg.option, std::move(status));
        }
    }

    for (const auto& item : items) {
        callback(std::get<0>(item), std::get<1>(item), std::get<2>(item), std::get<3>(item));
    }
}

std::string StreamPlugin::MakeKey(const std::string& type, const std::string& name) {
    return ToLowerAscii(type) + "." + name;
}

}  // namespace stream
}  // namespace flowsql
