#include "stream_plugin.h"

#include <framework/builtin/stream/tcp_session_mock_stream_channel.h>
#include <framework/core/ring_stream_channel.h>

#include <common/log.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <vector>
#include <yaml-cpp/yaml.h>

namespace flowsql {
namespace stream {

thread_local std::string StreamPlugin::last_error_;

namespace {

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

    if (config_file_.empty()) {
        return 0;
    }
    return LoadFromYamlLocked();
}

int StreamPlugin::Unload() {
    Stop();
    std::lock_guard<std::mutex> lock(mutex_);
    channels_.clear();
    configs_.clear();
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
        if (cfg.type == "ring") {
            RingStreamChannelOptions options;
            const int parse_rc = ParseRingOptions(cfg.option, &options);
            if (parse_rc != 0) {
                last_error_ = "invalid ring option for stream channel: " + cfg.type + "." + cfg.name;
                return -1;
            }

            auto channel = std::make_shared<RingStreamChannel>(cfg.type, cfg.name, options);
            const int open_rc = channel->Open();
            if (open_rc != 0) {
                last_error_ = "open stream channel failed: " + cfg.type + "." + cfg.name;
                return -1;
            }
            channels_[key] = channel;
            continue;
        }

        if (cfg.type == "tcp_session_mock") {
            TcpSessionMockOptions options;
            const int parse_rc = ParseTcpSessionMockOptions(cfg.option, &options);
            if (parse_rc != 0) {
                last_error_ = "invalid tcp_session_mock option for stream channel: " + cfg.type + "." + cfg.name;
                return -1;
            }

            auto channel = std::make_shared<TcpSessionMockStreamChannel>(cfg.type, cfg.name, options);
            const int open_rc = channel->Open();
            if (open_rc != 0) {
                last_error_ = "open stream channel failed: " + cfg.type + "." + cfg.name;
                return -1;
            }
            channels_[key] = channel;
            continue;
        }

        {
            // 路径 B 类型（如 netcard）本 Sprint 不实现，跳过加载。
            LOG_WARN("StreamPlugin::BuildChannelsLocked: skip unsupported stream type=%s name=%s",
                     cfg.type.c_str(), cfg.name.c_str());
        }
    }
    return 0;
}

std::string StreamPlugin::MakeKey(const std::string& type, const std::string& name) {
    return ToLowerAscii(type) + "." + name;
}

}  // namespace stream
}  // namespace flowsql
