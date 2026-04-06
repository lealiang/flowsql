#include "scheduler_plugin.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdio>
#include <chrono>
#include <common/error_code.h>
#include <common/log.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <regex>
#include <sstream>
#include <thread>
#include <type_traits>
#include <unordered_set>

#include "framework/core/channel_adapter.h"
#include "framework/core/dataframe.h"
#include "framework/core/dataframe_channel.h"
#include "framework/core/fan_in_stream_channel.h"
#include "framework/core/fan_out_stream_channel.h"
#include "framework/core/pipeline.h"
#include "framework/core/ring_stream_channel.h"
#include "framework/core/sql_parser.h"
#include "framework/core/sql_text_splitter.h"
#include "framework/interfaces/ichannel.h"
#include "framework/interfaces/ichannel_registry.h"
#include "framework/interfaces/idatabase_channel.h"
#include "framework/interfaces/idatabase_factory.h"
#include "framework/interfaces/idataframe_channel.h"
#include "framework/interfaces/ibuiltin_registry.h"
#include "framework/interfaces/ibridge.h"
#include "framework/interfaces/ioperator.h"
#include "framework/interfaces/ioperator_catalog.h"
#include "framework/interfaces/ioperator_registry.h"
#include "framework/interfaces/istream_channel.h"
#include "framework/interfaces/istream_factory.h"
#include "framework/interfaces/istream_manager.h"

namespace flowsql {
namespace scheduler {

static std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static bool IEquals(const std::string& a, const std::string& b) {
    return ToLowerAscii(a) == ToLowerAscii(b);
}

static bool StartsWithIgnoreCase(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

static bool IsDataFrameRef(const std::string& name) {
    return StartsWithIgnoreCase(name, "dataframe.") && name.size() > strlen("dataframe.");
}

static std::string DataFrameNamePart(const std::string& name) {
    if (!IsDataFrameRef(name)) return "";
    return name.substr(strlen("dataframe."));
}

static bool IsStreamRef(const std::string& name) {
    return StartsWithIgnoreCase(name, "stream.") && name.size() > strlen("stream.");
}

static std::string StreamNamePart(const std::string& name) {
    if (!IsStreamRef(name)) return "";
    return name.substr(strlen("stream."));
}

struct ParsedChannelRef {
    std::string raw;
    std::string base;
    bool has_selector = false;
    bool wildcard_selector = false;
    int selector_index = -1;
};

static bool ParseChannelRef(const std::string& raw,
                            ParsedChannelRef* out,
                            std::string* err) {
    if (!out) return false;
    if (err) err->clear();
    out->raw = raw;
    out->base.clear();
    out->has_selector = false;
    out->wildcard_selector = false;
    out->selector_index = -1;

    if (raw.empty()) {
        if (err) *err = "empty channel reference";
        return false;
    }

    const auto lb = raw.find('[');
    if (lb == std::string::npos) {
        out->base = raw;
        return true;
    }

    const auto rb = raw.find(']', lb + 1);
    if (rb == std::string::npos) {
        if (err) *err = "invalid channel selector: missing ']'";
        return false;
    }
    if (raw.find('[', rb + 1) != std::string::npos || rb + 1 != raw.size()) {
        if (err) *err = "invalid channel selector: duplicate selector is not allowed";
        return false;
    }

    out->base = raw.substr(0, lb);
    if (out->base.empty()) {
        if (err) *err = "invalid channel selector: empty base channel";
        return false;
    }
    const std::string selector = raw.substr(lb + 1, rb - lb - 1);
    out->has_selector = true;
    if (selector == "*") {
        out->wildcard_selector = true;
        return true;
    }
    if (selector.empty()) {
        if (err) *err = "invalid channel selector: expected '*' or index";
        return false;
    }
    for (char c : selector) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            if (err) *err = "invalid channel selector: expected '*' or index";
            return false;
        }
    }
    out->selector_index = std::atoi(selector.c_str());
    return true;
}

static bool ParseDatabaseDestination(const std::string& dest,
                                     std::string* db_type,
                                     std::string* db_name,
                                     std::string* table_name) {
    if (!db_type || !db_name || !table_name) return false;
    db_type->clear();
    db_name->clear();
    table_name->clear();

    const auto first = dest.find('.');
    if (first == std::string::npos || first == 0 || first >= dest.size() - 1) {
        return false;
    }
    *db_type = ToLowerAscii(dest.substr(0, first));

    const auto second = dest.find('.', first + 1);
    if (second == std::string::npos) {
        *db_name = dest.substr(first + 1);
        return !db_name->empty();
    }

    if (second == first + 1 || second >= dest.size() - 1) {
        return false;
    }
    *db_name = dest.substr(first + 1, second - first - 1);
    *table_name = dest.substr(second + 1);
    return !db_name->empty() && !table_name->empty();
}

static bool IsQualifiedDestination(const std::string& dest) {
    // 合法目标：
    // 1) dataframe.<name>
    // 2) type.name 或 type.name.table
    if (dest.empty()) return false;
    if (IsDataFrameRef(dest)) return true;
    const auto first = dest.find('.');
    if (first == std::string::npos || first == 0 || first == dest.size() - 1) return false;
    const auto second = dest.find('.', first + 1);
    if (second == first + 1) return false;
    if (second != std::string::npos && second == dest.size() - 1) return false;
    return true;
}

static int32_t MapStreamManagerErrorToStatus(int rc) {
    if (rc == 0) return error::OK;
    if (rc == EEXIST || rc == EBUSY) return error::CONFLICT;
    if (rc == ENOENT) return error::NOT_FOUND;
    if (rc == EINVAL) return error::BAD_REQUEST;
    if (rc == ENOTSUP) return error::BAD_REQUEST;
    return error::INTERNAL_ERROR;
}

static std::string MakeStreamChannelKey(const std::string& type, const std::string& name) {
    return ToLowerAscii(type) + "." + name;
}

static std::string NormalizeStreamRole(const std::string& role) {
    const std::string lower = ToLowerAscii(role);
    if (lower == "source" || lower == "sink" || lower == "both") return lower;
    return "";
}

static bool IsSourceRoleAllowed(const std::string& role) {
    const std::string normalized = NormalizeStreamRole(role);
    return normalized == "source" || normalized == "both";
}

static bool IsSinkRoleAllowed(const std::string& role) {
    const std::string normalized = NormalizeStreamRole(role);
    return normalized == "sink" || normalized == "both";
}

static std::shared_ptr<IChannel> MakeNonOwningChannelHolder(IChannel* ch) {
    if (!ch) return nullptr;
    return std::shared_ptr<IChannel>(ch, [](IChannel*) {});
}

static std::shared_ptr<IStreamChannel> MakeStreamOwner(IStreamChannel* stream_ch,
                                                       const std::shared_ptr<IChannel>& owner) {
    if (!stream_ch) return nullptr;
    if (owner) {
        auto stream_owner = std::dynamic_pointer_cast<IStreamChannel>(owner);
        if (stream_owner) return stream_owner;
    }
    return std::shared_ptr<IStreamChannel>(stream_ch, [](IStreamChannel*) {});
}

static int ParseOptionObject(const std::string& option, rapidjson::Document* out, std::string* err) {
    if (!out) return EINVAL;
    out->SetObject();
    auto& alloc = out->GetAllocator();
    if (option.empty()) return 0;

    const std::string trimmed = [&option]() {
        size_t begin = 0;
        size_t end = option.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(option[begin]))) ++begin;
        while (end > begin && std::isspace(static_cast<unsigned char>(option[end - 1]))) --end;
        return option.substr(begin, end - begin);
    }();

    if (trimmed.empty()) return 0;
    if (trimmed.front() == '{') {
        out->Parse(trimmed.c_str());
        if (out->HasParseError() || !out->IsObject()) {
            if (err) *err = "option json parse failed";
            return EINVAL;
        }
        return 0;
    }

    size_t pos = 0;
    while (pos < trimmed.size()) {
        const size_t eq = trimmed.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = trimmed.find(';', eq + 1);
        if (end == std::string::npos) end = trimmed.size();

        const std::string key = trimmed.substr(pos, eq - pos);
        const std::string value = trimmed.substr(eq + 1, end - eq - 1);
        if (!key.empty()) {
            rapidjson::Value key_json(key.c_str(), alloc);
            const std::string lower = ToLowerAscii(value);
            bool is_int = !value.empty();
            size_t i = 0;
            if (is_int && (value[0] == '+' || value[0] == '-')) i = 1;
            if (i >= value.size()) is_int = false;
            for (; is_int && i < value.size(); ++i) {
                if (!std::isdigit(static_cast<unsigned char>(value[i]))) {
                    is_int = false;
                }
            }
            if (lower == "true" || lower == "false") {
                out->AddMember(key_json, rapidjson::Value(lower == "true"), alloc);
            } else if (is_int) {
                const int64_t num = static_cast<int64_t>(std::strtoll(value.c_str(), nullptr, 10));
                out->AddMember(key_json, rapidjson::Value(num), alloc);
            } else {
                out->AddMember(key_json, rapidjson::Value(value.c_str(), alloc), alloc);
            }
        }
        pos = (end < trimmed.size()) ? end + 1 : trimmed.size();
    }
    return 0;
}

static std::string OptionObjectToJson(const rapidjson::Value& option_obj) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    option_obj.Accept(w);
    return buf.GetString();
}

static std::string BuildOptionWithRoleJson(const rapidjson::Value* options,
                                           const std::string& role) {
    rapidjson::Document d;
    d.SetObject();
    auto& alloc = d.GetAllocator();
    if (options && options->IsObject()) {
        for (auto it = options->MemberBegin(); it != options->MemberEnd(); ++it) {
            rapidjson::Value key;
            key.SetString(it->name.GetString(), alloc);
            rapidjson::Value value;
            value.CopyFrom(it->value, alloc);
            d.AddMember(key, value, alloc);
        }
    }
    rapidjson::Value role_key("role", alloc);
    rapidjson::Value role_value(role.c_str(), alloc);
    if (d.HasMember("role")) {
        d["role"] = role_value;
    } else {
        d.AddMember(role_key, role_value, alloc);
    }
    return OptionObjectToJson(d);
}

static std::string ReadRoleFromOption(const std::string& option) {
    rapidjson::Document d;
    std::string parse_err;
    if (ParseOptionObject(option, &d, &parse_err) != 0 || !d.IsObject()) {
        return "both";
    }
    if (!d.HasMember("role") || !d["role"].IsString()) return "both";
    const std::string role = NormalizeStreamRole(d["role"].GetString());
    return role.empty() ? "both" : role;
}

// --- JSON 辅助 ---
static std::string MakeErrorJson(const std::string& error) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error.c_str());
    w.EndObject();
    return buf.GetString();
}

static std::string MakeExecutionErrorJson(const std::string& error,
                                          const std::string& error_code,
                                          const std::string& error_stage) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error.c_str());
    w.Key("error_code");
    w.String(error_code.c_str());
    w.Key("error_stage");
    w.String(error_stage.c_str());
    w.EndObject();
    return buf.GetString();
}

static std::string MakeExecutionErrorWithSqlIndexJson(const std::string& error,
                                                      const std::string& error_code,
                                                      const std::string& error_stage,
                                                      size_t sql_index) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error.c_str());
    w.Key("error_code");
    w.String(error_code.c_str());
    w.Key("error_stage");
    w.String(error_stage.c_str());
    w.Key("sql_index");
    w.Uint64(static_cast<uint64_t>(sql_index));
    w.EndObject();
    return buf.GetString();
}

static std::string ExtractStageFromExecutionError(const std::string& error) {
    // Pipeline::Run 失败消息：operator <category>.<name> execution failed
    static const std::regex kPattern(R"(^operator\s+([^.]+)\.([^\s]+)\s+execution failed$)",
                                     std::regex_constants::icase);
    std::smatch m;
    if (!std::regex_match(error, m, kPattern)) return "";
    if (m.size() < 3) return "";
    return m[2].str();
}

static const char* DataTypeName(DataType t) {
    switch (t) {
        case DataType::INT32: return "INT32";
        case DataType::INT64: return "INT64";
        case DataType::UINT32: return "UINT32";
        case DataType::UINT64: return "UINT64";
        case DataType::FLOAT: return "FLOAT";
        case DataType::DOUBLE: return "DOUBLE";
        case DataType::STRING: return "STRING";
        case DataType::BYTES: return "BYTES";
        case DataType::TIMESTAMP: return "TIMESTAMP";
        case DataType::BOOLEAN: return "BOOLEAN";
        default: return "UNKNOWN";
    }
}

static int64_t CurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string MakeWithParamsJson(const std::unordered_map<std::string, std::string>& params) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    for (const auto& kv : params) {
        w.Key(kv.first.c_str());
        w.String(kv.second.c_str());
    }
    w.EndObject();
    return buf.GetString();
}

static size_t NextPowerOfTwo(size_t value) {
    if (value <= 1) return 1;
    size_t v = value - 1;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    if (sizeof(size_t) >= 8) {
        v |= v >> 32;
    }
    return v + 1;
}

static const char* StreamTaskStatusName(StreamTaskStatus status) {
    switch (status) {
        case StreamTaskStatus::kCreated: return "created";
        case StreamTaskStatus::kRunning: return "running";
        case StreamTaskStatus::kStopping: return "stopping";
        case StreamTaskStatus::kStopped: return "stopped";
        case StreamTaskStatus::kCancelled: return "cancelled";
        case StreamTaskStatus::kFailed: return "failed";
        default: return "unknown";
    }
}

static bool IsTerminalStreamTaskStatus(StreamTaskStatus status) {
    return status == StreamTaskStatus::kStopped ||
           status == StreamTaskStatus::kCancelled ||
           status == StreamTaskStatus::kFailed;
}

static const char* ProducerModeName(ProducerMode mode) {
    return mode == ProducerMode::MULTI ? "MULTI" : "SINGLE";
}

static const char* ConsumerModeName(ConsumerMode mode) {
    return mode == ConsumerMode::MULTI ? "MULTI" : "SINGLE";
}

static void WriteCapabilitiesObject(rapidjson::Writer<rapidjson::StringBuffer>* w,
                                    const StreamChannelCapabilities& caps) {
    if (!w) return;
    w->StartObject();
    w->Key("channel_type");
    w->String(caps.channel_type.c_str());
    w->Key("concurrency");
    w->StartObject();
    w->Key("put_mode");
    w->String(ProducerModeName(caps.concurrency.put_mode));
    w->Key("poll_mode");
    w->String(ConsumerModeName(caps.concurrency.poll_mode));
    w->Key("max_producers");
    w->Uint(caps.concurrency.max_producers);
    w->Key("max_consumers");
    w->Uint(caps.concurrency.max_consumers);
    w->Key("lock_free_put");
    w->Bool(caps.concurrency.lock_free_put);
    w->Key("lock_free_poll");
    w->Bool(caps.concurrency.lock_free_poll);
    w->Key("cancel_wakeup_guaranteed");
    w->Bool(caps.concurrency.cancel_wakeup_guaranteed);
    w->EndObject();
    w->EndObject();
}

static std::string MakeCapabilityMismatchJson(const std::string& error_message,
                                              const std::string& error_code,
                                              const StreamChannelCapabilities* source_caps,
                                              const StreamChannelCapabilities* sink_caps) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error_message.c_str());
    w.Key("error_code");
    w.String(error_code.c_str());
    w.Key("error_stage");
    w.String("capability_check");
    w.Key("details");
    w.StartObject();
    w.Key("capabilities");
    w.StartObject();
    if (source_caps) {
        w.Key("source");
        WriteCapabilitiesObject(&w, *source_caps);
    }
    if (sink_caps) {
        w.Key("sink");
        WriteCapabilitiesObject(&w, *sink_caps);
    }
    w.EndObject();
    w.EndObject();
    w.EndObject();
    return buf.GetString();
}

static void WriteTaskSnapshotJson(rapidjson::Writer<rapidjson::StringBuffer>* w,
                                  const TaskSnapshot& s) {
    if (!w) return;
    w->StartObject();
    w->Key("task_id");
    w->String(s.task_id.c_str());
    w->Key("runtime_task_id");
    w->String(s.task_id.c_str());
    w->Key("runtime_kind");
    w->String("single");
    w->Key("status");
    w->String(StreamTaskStatusName(s.status));
    w->Key("stop_requested");
    w->Bool(s.stop_requested);
    w->Key("joined");
    w->Bool(s.joined);
    w->Key("shard_count");
    w->Uint(s.shard_count);
    w->Key("active_shards");
    w->Uint(s.active_shards);

    w->Key("processed_batches");
    w->Uint64(s.processed_batches);
    w->Key("processed_rows");
    w->Uint64(s.processed_rows);
    w->Key("processed_bytes");
    w->Uint64(s.processed_bytes);
    w->Key("output_rows");
    w->Uint64(s.output_rows);
    w->Key("output_batches");
    w->Uint64(s.output_batches);
    w->Key("dropped_batches");
    w->Uint64(s.dropped_batches);
    w->Key("poll_timeouts");
    w->Uint64(s.poll_timeouts);
    w->Key("poll_errors");
    w->Uint64(s.poll_errors);
    w->Key("queue_depth");
    w->Uint64(s.queue_depth);
    w->Key("queue_depth_peak");
    w->Uint64(s.queue_depth_peak);
    w->Key("uptime_ms");
    w->Int64(s.uptime_ms);
    w->Key("started_ms");
    w->Int64(s.started_ms);
    w->Key("last_active_ms");
    w->Int64(s.last_active_ms);
    w->Key("finished_ms");
    w->Int64(s.finished_ms);
    w->Key("error_code");
    w->Int(s.error_code);
    w->Key("error_message");
    w->String(s.error_message.c_str());
    w->Key("resolved_sources");
    w->StartArray();
    for (const auto& source : s.resolved_sources) {
        w->String(source.c_str());
    }
    w->EndArray();
    w->Key("source_expand_rule");
    w->String(s.source_expand_rule.c_str());

    rapidjson::Document stats_doc;
    if (!s.op_stats_json.empty()) {
        stats_doc.Parse(s.op_stats_json.c_str());
    }
    w->Key("op_stats");
    if (stats_doc.HasParseError()) {
        w->String(s.op_stats_json.c_str());
    } else {
        rapidjson::StringBuffer stats_buf;
        rapidjson::Writer<rapidjson::StringBuffer> stats_writer(stats_buf);
        stats_doc.Accept(stats_writer);
        w->RawValue(stats_buf.GetString(), stats_buf.GetSize(),
                    stats_doc.IsArray() ? rapidjson::kArrayType : rapidjson::kObjectType);
    }
    w->EndObject();
}

static void WriteGroupSnapshotJson(rapidjson::Writer<rapidjson::StringBuffer>* w,
                                   const StreamGroupSnapshot& s,
                                   const std::vector<BroadcastHubSnapshot>* share_sets,
                                   const std::unordered_map<std::string, GroupNodeResolvedSourceMeta>* node_sources) {
    if (!w) return;
    uint64_t processed_rows = 0;
    uint64_t output_rows = 0;
    uint64_t dropped_batches = 0;
    uint64_t poll_errors = 0;
    for (const auto& node : s.nodes) {
        processed_rows += node.processed_rows;
        output_rows += node.output_rows;
        dropped_batches += node.dropped_batches;
        poll_errors += node.poll_errors;
    }

    w->StartObject();
    w->Key("task_id");
    w->String(s.task_id.c_str());
    w->Key("runtime_task_id");
    w->String(s.runtime_task_id.c_str());
    w->Key("runtime_kind");
    w->String("group");
    w->Key("group_mode");
    w->String(s.group_mode.c_str());
    w->Key("status");
    w->String(StreamGroupStatusName(s.status));
    w->Key("group_status");
    w->String(StreamGroupStatusName(s.status));
    w->Key("stop_requested");
    w->Bool(s.stop_requested);
    w->Key("joined");
    w->Bool(IsTerminalStreamGroupStatus(s.status));
    w->Key("node_count");
    w->Uint(s.node_count);
    w->Key("active_nodes");
    w->Uint(s.active_nodes);
    w->Key("share_set_count");
    w->Uint(static_cast<unsigned>(share_sets ? share_sets->size() : 0));
    w->Key("processed_rows");
    w->Uint64(processed_rows);
    w->Key("output_rows");
    w->Uint64(output_rows);
    w->Key("dropped_batches_shared");
    w->Uint64(dropped_batches);
    w->Key("poll_errors");
    w->Uint64(poll_errors);
    w->Key("started_ms");
    w->Int64(s.started_ms);
    w->Key("last_active_ms");
    w->Int64(s.last_active_ms);
    w->Key("finished_ms");
    w->Int64(s.finished_ms);
    w->Key("error_code");
    w->String(s.error_code.c_str());
    w->Key("error_no");
    w->Int(s.error_no);
    w->Key("error_message");
    w->String(s.error_message.c_str());

    w->Key("resolved_sources");
    w->StartArray();
    for (const auto& node : s.nodes) {
        w->StartObject();
        w->Key("node_id");
        w->String(node.node_id.c_str());
        const GroupNodeResolvedSourceMeta* source_meta = nullptr;
        if (node_sources) {
            auto it = node_sources->find(node.node_id);
            if (it != node_sources->end()) {
                source_meta = &it->second;
            }
        }
        w->Key("sources");
        w->StartArray();
        if (source_meta) {
            for (const auto& source : source_meta->sources) {
                w->String(source.c_str());
            }
        }
        w->EndArray();
        w->Key("expand_rule");
        w->String(source_meta ? source_meta->expand_rule.c_str() : "explicit");
        w->EndObject();
    }
    w->EndArray();

    w->Key("nodes");
    w->StartArray();
    for (const auto& node : s.nodes) {
        w->StartObject();
        w->Key("id");
        w->String(node.node_id.c_str());
        w->Key("runtime_task_id");
        w->String(node.runtime_task_id.c_str());
        w->Key("status");
        w->String(GroupNodeStatusName(node.status));
        w->Key("depends_on");
        w->StartArray();
        for (const auto& dep : node.depends_on) {
            w->String(dep.c_str());
        }
        w->EndArray();
        w->Key("start_condition");
        w->String(GroupStartConditionName(node.start_condition));
        w->Key("processed_rows");
        w->Uint64(node.processed_rows);
        w->Key("output_rows");
        w->Uint64(node.output_rows);
        w->Key("dropped_batches");
        w->Uint64(node.dropped_batches);
        w->Key("poll_errors");
        w->Uint64(node.poll_errors);
        w->Key("error_code");
        w->String(node.error_code.c_str());
        w->Key("error_no");
        w->Int(node.error_no);
        w->Key("last_error");
        w->String(node.error_message.c_str());
        w->Key("started_ms");
        w->Int64(node.started_ms);
        w->Key("last_active_ms");
        w->Int64(node.last_active_ms);
        w->Key("finished_ms");
        w->Int64(node.finished_ms);
        w->EndObject();
    }
    w->EndArray();

    w->Key("share_sets");
    w->StartArray();
    if (share_sets) {
        for (const auto& ss : *share_sets) {
            w->StartObject();
            w->Key("id");
            w->String(ss.id.c_str());
            w->Key("source_ref");
            w->String(ss.source_ref.c_str());
            w->Key("status");
            w->String(BroadcastHubStatusName(ss.status));
            w->Key("members");
            w->StartArray();
            for (const auto& member : ss.members) {
                w->String(member.c_str());
            }
            w->EndArray();
            w->Key("input_batches");
            w->Uint64(ss.input_batches);
            w->Key("delivered_batches");
            w->Uint64(ss.delivered_batches);
            w->Key("dropped_batches_shared");
            w->Uint64(ss.dropped_batches_shared);
            w->Key("drop_ratio");
            w->Double(ss.drop_ratio);
            w->Key("input_rows");
            w->Uint64(ss.input_rows);
            w->Key("delivered_rows");
            w->Uint64(ss.delivered_rows);
            w->Key("dropped_rows_shared");
            w->Uint64(ss.dropped_rows_shared);
            w->Key("last_delivered_seq");
            w->Uint64(ss.last_delivered_seq);
            w->Key("last_dropped_seq");
            w->Uint64(ss.last_dropped_seq);
            w->Key("error_code");
            w->Int(ss.error_code);
            w->Key("error_message");
            w->String(ss.error_message.c_str());
            w->EndObject();
        }
    }
    w->EndArray();

    w->EndObject();
}

class SharedSourceState final : public std::enable_shared_from_this<SharedSourceState> {
 public:
    explicit SharedSourceState(std::shared_ptr<IStreamChannel> source)
        : source_(std::move(source)) {}

    PollEvent PollNext(int timeout_ms) {
        if (!source_) return PollEvent::Error(-EINVAL, "shared source unavailable");
        return source_->PollNext(timeout_ms);
    }

    void Cancel() {
        std::call_once(cancel_once_, [this]() {
            if (source_) source_->Cancel();
        });
    }

    int CloseView() {
        const int remain = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remain == 0 && source_) {
            return source_->Close();
        }
        return 0;
    }

    bool IsFinished() const {
        return source_ ? source_->IsFinished() : true;
    }

    bool IsFull() const {
        return source_ && source_->IsFull();
    }

    bool IsEmpty() const {
        return !source_ || source_->IsEmpty();
    }

    size_t Capacity() const {
        return source_ ? source_->Capacity() : 0;
    }

    size_t Size() const {
        return source_ ? source_->Size() : 0;
    }

    std::shared_ptr<arrow::Schema> GetOutputSchema() {
        return source_ ? source_->GetOutputSchema() : nullptr;
    }

    StreamChannelCapabilities Capabilities() const {
        return source_ ? source_->Capabilities() : StreamChannelCapabilities{};
    }

    const char* Category() const { return source_ ? source_->Category() : "stateless"; }
    const char* Name() const { return source_ ? source_->Name() : "stateless"; }
    const char* Schema() const { return source_ ? source_->Schema() : "[]"; }
    bool IsFinite() const { return source_ && source_->IsFinite(); }

 private:
    std::shared_ptr<IStreamChannel> source_;
    std::atomic<int> refs_{0};
    std::once_flag cancel_once_;

    friend class StatelessSourceView;
};

class StatelessSourceView final : public IStreamChannel {
 public:
    StatelessSourceView(std::shared_ptr<SharedSourceState> state, uint32_t view_id)
        : state_(std::move(state)),
          view_name_(state_ ? std::string(state_->Name()) + ".sv" + std::to_string(view_id)
                            : ("stateless.sv" + std::to_string(view_id))) {
        if (state_) {
            state_->refs_.fetch_add(1, std::memory_order_release);
        }
    }
    ~StatelessSourceView() override { (void)Close(); }

    const char* Category() override {
        return state_ ? state_->Category() : "stateless";
    }

    const char* Name() override {
        return view_name_.c_str();
    }

    const char* Type() override {
        return ChannelType::kStream;
    }

    const char* Schema() override {
        return state_ ? state_->Schema() : "[]";
    }

    int Open() override { return 0; }
    int Close() override {
        bool expected = false;
        if (!closed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return 0;
        }
        return state_ ? state_->CloseView() : 0;
    }
    bool IsOpened() const override { return true; }
    int Flush() override { return 0; }

    int Put(std::shared_ptr<arrow::RecordBatch>, int64_t) override {
        return ENOTSUP;
    }

    PollEvent PollNext(int timeout_ms = 100) override {
        if (!state_) return PollEvent::Error(-EINVAL, "invalid shared spmc state");
        return state_->PollNext(timeout_ms);
    }

    std::shared_ptr<arrow::Schema> GetOutputSchema() override {
        return state_ ? state_->GetOutputSchema() : nullptr;
    }

    int SetFilter(const char*, std::vector<std::string>* unsupported_out) override {
        if (unsupported_out) unsupported_out->clear();
        return 0;
    }

    StreamChannelCapabilities Capabilities() const override {
        return state_ ? state_->Capabilities() : StreamChannelCapabilities{};
    }

    bool IsFull() const override { return state_ && state_->IsFull(); }
    bool IsEmpty() const override { return !state_ || state_->IsEmpty(); }
    size_t Capacity() const override { return state_ ? state_->Capacity() : 0; }
    size_t Size() const override { return state_ ? state_->Size() : 0; }
    bool IsFinite() const override { return state_ && state_->IsFinite(); }
    void CloseStream() override {}
    void Cancel() override {
        if (state_) state_->Cancel();
    }
    bool IsFinished() const override {
        return state_ && state_->IsFinished();
    }

 private:
    std::shared_ptr<SharedSourceState> state_;
    std::string view_name_;
    std::atomic<bool> closed_{false};
};

class FanOutPartitionView final : public IStreamChannel {
 public:
    FanOutPartitionView(std::shared_ptr<FanOutStreamChannel> parent,
                        std::shared_ptr<IStreamChannel> partition)
        : parent_(std::move(parent)),
          partition_(std::move(partition)) {}

    const char* Category() override {
        return partition_ ? partition_->Category() : "fanout";
    }

    const char* Name() override {
        return partition_ ? partition_->Name() : "fanout.partition";
    }

    const char* Type() override {
        return ChannelType::kStream;
    }

    const char* Schema() override {
        return partition_ ? partition_->Schema() : "[]";
    }

    int Open() override { return 0; }
    int Close() override { return partition_ ? partition_->Close() : 0; }
    bool IsOpened() const override { return partition_ && partition_->IsOpened(); }
    int Flush() override { return partition_ ? partition_->Flush() : 0; }

    int Put(std::shared_ptr<arrow::RecordBatch> batch, int64_t ts_ms) override {
        return partition_ ? partition_->Put(std::move(batch), ts_ms) : EINVAL;
    }

    PollEvent PollNext(int timeout_ms = 100) override {
        if (!partition_) return PollEvent::Error(-EINVAL, "fanout partition unavailable");
        return partition_->PollNext(timeout_ms);
    }

    std::shared_ptr<arrow::Schema> GetOutputSchema() override {
        return partition_ ? partition_->GetOutputSchema() : nullptr;
    }

    int SetFilter(const char* condition_json,
                  std::vector<std::string>* unsupported_out) override {
        return partition_ ? partition_->SetFilter(condition_json, unsupported_out) : EINVAL;
    }

    bool IsFull() const override { return partition_ && partition_->IsFull(); }
    bool IsEmpty() const override { return !partition_ || partition_->IsEmpty(); }
    size_t Capacity() const override { return partition_ ? partition_->Capacity() : 0; }
    size_t Size() const override { return partition_ ? partition_->Size() : 0; }
    bool IsFinite() const override { return partition_ && partition_->IsFinite(); }
    void CloseStream() override {
        if (partition_) partition_->CloseStream();
    }
    void Cancel() override {
        if (partition_) partition_->Cancel();
        if (parent_) parent_->Cancel();
    }
    bool IsFinished() const override { return partition_ && partition_->IsFinished(); }

 private:
    std::shared_ptr<FanOutStreamChannel> parent_;
    std::shared_ptr<IStreamChannel> partition_;
};

// --- IPlugin ---
int SchedulerPlugin::Option(const char* arg) {
    if (!arg) return 0;

    std::string opts(arg);
    size_t pos = 0;
    while (pos < opts.size()) {
        size_t eq = opts.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = opts.find(';', eq);
        if (end == std::string::npos) end = opts.size();

        std::string key = opts.substr(pos, eq - pos);
        std::string val = opts.substr(eq + 1, end - eq - 1);

        if (key == "host") host_ = val;
        else if (key == "port") port_ = std::stoi(val);
        else if (key == "stream_workers") stream_worker_count_ = static_cast<size_t>(std::stoull(val));
        else if (key == "max_resolved_sources") {
            const size_t parsed = static_cast<size_t>(std::stoull(val));
            max_resolved_sources_ = std::max<size_t>(1, parsed);
        } else if (key == "max_stream_group_timeout_s") {
            const int parsed = std::stoi(val);
            max_stream_group_timeout_s_ = std::max(1, parsed);
        }

        pos = (end < opts.size()) ? end + 1 : opts.size();
    }
    return 0;
}

int SchedulerPlugin::Load(IQuerier* querier) {
    querier_ = querier;
    LOG_INFO("SchedulerPlugin::Load: host=%s, port=%d", host_.c_str(), port_);
    return 0;
}

int SchedulerPlugin::Unload() {
    return 0;
}

// --- 通道管理 ---
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

// --- IPlugin::Start ---
int SchedulerPlugin::Start() {
    auto* catalog = querier_ ? static_cast<IOperatorCatalog*>(querier_->First(IID_OPERATOR_CATALOG)) : nullptr;
    if (catalog) {
        std::vector<OperatorMeta> ops;
        std::unordered_set<std::string> seen;
        auto append_unique = [&](OperatorMeta meta) {
            const std::string key = ToLowerAscii(meta.category) + "." + ToLowerAscii(meta.name);
            if (seen.insert(key).second) {
                ops.push_back(std::move(meta));
            }
        };
        querier_->Traverse(IID_OPERATOR, [&](void* p) -> int {
            auto* op = static_cast<IOperator*>(p);
            if (!op || op->Category().empty() || op->Name().empty()) return 0;
            OperatorMeta meta;
            meta.category = op->Category();
            meta.name = op->Name();
            meta.type = IEquals(meta.category, "builtin") ? "builtin" : "cpp";
            meta.source = "scheduler";
            meta.description = op->Description();
            meta.position = op->Position() == OperatorPosition::STORAGE ? "storage" : "data";
            append_unique(std::move(meta));
            return 0;
        });
        auto* op_registry = static_cast<IOperatorRegistry*>(querier_->First(IID_OPERATOR_REGISTRY));
        if (op_registry) {
            op_registry->List([&](const char* name) {
                if (!name || name[0] == '\0') return;
                if (std::string(name).find('.') != std::string::npos) return;
                OperatorMeta meta;
                meta.category = "builtin";
                meta.name = name;
                meta.type = "builtin";
                meta.source = "scheduler";
                meta.position = "data";
                IOperator* op = op_registry->Create(name);
                if (op) {
                    meta.description = op->Description();
                    meta.position = op->Position() == OperatorPosition::STORAGE ? "storage" : "data";
                    delete op;
                }
                append_unique(std::move(meta));
            });
        }
        UpsertResult upsert = catalog->UpsertBatch(ops);
        if (upsert.failed_count > 0) {
            LOG_ERROR("SchedulerPlugin::Start: catalog upsert failed, success=%d failed=%d err=%s",
                      upsert.success_count, upsert.failed_count, upsert.error_message.c_str());
        } else {
            LOG_INFO("SchedulerPlugin::Start: synced %d C++ operators to Catalog", upsert.success_count);
        }
    } else {
        LOG_ERROR("SchedulerPlugin::Start: IOperatorCatalog not found");
    }

    size_t workers = stream_worker_count_;
    if (workers == 0) {
        workers = static_cast<size_t>(std::thread::hardware_concurrency());
    }
    if (workers == 0) workers = 1;
    stream_runtime_.Start(workers);

    LOG_INFO("SchedulerPlugin::Start: ready, stream_workers=%zu", workers);
    return 0;
}

int SchedulerPlugin::Stop() {
    std::vector<std::pair<std::string, std::shared_ptr<StreamTaskGroup>>> groups;
    {
        std::lock_guard<std::mutex> lock(stream_task_groups_mu_);
        groups.reserve(stream_task_groups_.size());
        for (const auto& kv : stream_task_groups_) {
            groups.push_back(kv);
        }
    }
    for (const auto& entry : groups) {
        if (entry.second) entry.second->RequestStop();
    }
    for (const auto& entry : groups) {
        if (entry.second) entry.second->Join();
    }
    for (const auto& entry : groups) {
        StreamGroupSnapshot snapshot;
        StreamGroupSnapshot* snapshot_ptr = nullptr;
        if (entry.second) {
            snapshot = entry.second->Snapshot();
            snapshot_ptr = &snapshot;
        }
        CleanupGroupRuntimeResources(entry.first, snapshot_ptr);
    }

    std::vector<std::shared_ptr<StreamTask>> tasks;
    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        tasks.reserve(stream_tasks_.size());
        for (const auto& kv : stream_tasks_) {
            if (kv.second) tasks.push_back(kv.second);
        }
    }

    for (const auto& task : tasks) {
        const StreamTaskStatus st = task->Status();
        if (st == StreamTaskStatus::kRunning ||
            st == StreamTaskStatus::kStopping ||
            st == StreamTaskStatus::kCreated) {
            task->RequestStop();
        }
    }
    for (const auto& task : tasks) {
        task->Join();
    }
    stream_runtime_.Stop();

    {
        std::lock_guard<std::mutex> lock(stream_task_groups_mu_);
        stream_task_groups_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(stream_group_nodes_mu_);
        stream_group_node_owners_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(stream_group_node_sources_mu_);
        stream_group_node_sources_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(stream_group_share_sets_mu_);
        stream_group_share_sets_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(stream_group_share_set_snapshots_mu_);
        stream_group_share_set_snapshots_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        stream_tasks_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(stream_channel_refs_mu_);
        stream_task_leases_.clear();
        stream_source_leases_.clear();
        stream_channel_ref_counts_.clear();
        stream_channel_mutating_.clear();
    }
    ClearManagedChannels();
    LOG_INFO("SchedulerPlugin::Stop: done");
    return 0;
}

// --- IRouterHandle ---
void SchedulerPlugin::EnumRoutes(std::function<void(const RouteItem&)> cb) {
    // 任务执行
    cb({"POST", "/scheduler/batch/execute",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleExecute(u, req, rsp);
        }});
    cb({"POST", "/scheduler/sql/classify",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleSqlClassify(u, req, rsp);
        }});
    cb({"POST", "/scheduler/stream/execute",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleStreamExecute(u, req, rsp);
        }});
    cb({"POST", "/scheduler/stream/stop",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleStreamStop(u, req, rsp);
        }});
    cb({"POST", "/scheduler/stream/status",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleStreamStatus(u, req, rsp);
        }});
    cb({"POST", "/scheduler/stream/list",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleStreamList(u, req, rsp);
        }});
    // 流式通道查询（管理面最小字段）
    cb({"POST", "/channels/stream/query",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleQueryStreamChannels(u, req, rsp);
        }});
    cb({"POST", "/channels/stream/definitions/query",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleQueryStreamChannelDefinitions(u, req, rsp);
        }});
    cb({"POST", "/channels/stream/add",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleAddStreamChannel(u, req, rsp);
        }});
    cb({"POST", "/channels/stream/modify",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleModifyStreamChannel(u, req, rsp);
        }});
    cb({"POST", "/channels/stream/remove",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleRemoveStreamChannel(u, req, rsp);
        }});
    // 内存通道查询
    cb({"POST", "/channels/dataframe/query",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleGetChannels(u, req, rsp);
        }});
    // 内存通道数据预览
    cb({"POST", "/channels/dataframe/preview",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandlePreviewDataframe(u, req, rsp);
        }});
    // Python 算子刷新
    cb({"POST", "/operators/python/refresh",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleRefreshOperators(u, req, rsp);
        }});
}

// --- 通道查找辅助 ---
IChannel* SchedulerPlugin::FindChannel(const std::string& name) {
    return FindChannel(name, nullptr);
}

IChannel* SchedulerPlugin::FindChannel(const std::string& name, std::shared_ptr<IChannel>* owner_out) {
    if (owner_out) owner_out->reset();
    auto* ch_registry = querier_ ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY)) : nullptr;
    if (IsDataFrameRef(name) && ch_registry) {
        auto ch = ch_registry->Get(DataFrameNamePart(name).c_str());
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

    if (IsStreamRef(name) && querier_) {
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
                if (owner_out) *owner_out = MakeNonOwningChannelHolder(matched);
                return matched;
            }
        }
    }

    // 通过 IQuerier 遍历静态注册的通道
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
                return -1;  // 找到了，停止遍历
            }
            return 0;
        });
    }
    if (found && owner_out) *owner_out = MakeNonOwningChannelHolder(found);

    // 模糊匹配内部通道表
    if (!found) {
        // 【第四层】尝试通过 IDatabaseFactory 获取数据库通道
        // 支持三段式（type.name.table）和两段式（type.name）
        if (querier_) {
            auto* factory = static_cast<IDatabaseFactory*>(
                querier_->First(IID_DATABASE_FACTORY));
            if (factory) {
                // 尝试解析 type.name 格式
                auto pos = name.find('.');
                if (pos != std::string::npos) {
                    std::string type = ToLowerAscii(name.substr(0, pos));
                    std::string rest = name.substr(pos + 1);
                    // 对于三段式 type.name.table，取前两段作为 type.name
                    auto pos2 = rest.find('.');
                    std::string db_name = (pos2 != std::string::npos) ? rest.substr(0, pos2) : rest;

                    auto* db_ch = factory->Get(type.c_str(), db_name.c_str());
                    if (db_ch) found = db_ch;
                }
            }
        }
    }
    if (found && owner_out && !*owner_out) *owner_out = MakeNonOwningChannelHolder(found);

    // 尝试通过 IStreamFactory 获取流式通道（type.name）
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
    if (found && owner_out && !*owner_out) *owner_out = MakeNonOwningChannelHolder(found);

    return found;
}

// --- 算子查找 ---
// 先查 C++ 静态算子（IQuerier），再查 Python 算子（IBridge）
std::shared_ptr<IOperator> SchedulerPlugin::FindOperator(const std::string& category, const std::string& name) {
    if (!querier_) return nullptr;
    auto* op_registry = static_cast<IOperatorRegistry*>(querier_->First(IID_OPERATOR_REGISTRY));

    // 1. 先查 C++ 静态算子
    IOperator* found = nullptr;
    querier_->Traverse(IID_OPERATOR, [&](void* p) -> int {
        auto* op = static_cast<IOperator*>(p);
        if (IEquals(op->Category(), category) && op->Name() == name) {
            found = op;
            return -1;
        }
        return 0;
    });
    // C++ 算子由 PluginLoader 管理生命周期，用空 deleter 包装
    if (found) return std::shared_ptr<IOperator>(found, [](IOperator*) {});

    // 2. 再查 Python 算子（通过 IBridge）
    auto* bridge = static_cast<IBridge*>(querier_->First(IID_BRIDGE));
    if (bridge) {
        auto py_op = bridge->FindOperator(category, name);
        if (py_op) return py_op;
    }

    // 3. 查注册表（兼容 "category.name" 与 legacy builtin name）
    if (op_registry) {
        const std::string key = category + "." + name;
        IOperator* op = op_registry->Create(key.c_str());
        if (op) return std::shared_ptr<IOperator>(op, [](IOperator* p) { delete p; });
    }
    if (IEquals(category, "builtin") && op_registry) {
        IOperator* op = op_registry->Create(name.c_str());
        if (op) return std::shared_ptr<IOperator>(op, [](IOperator* p) { delete p; });
    }
    return nullptr;
}

std::shared_ptr<IOperator> SchedulerPlugin::CreateOperator(const std::string& category,
                                                           const std::string& name) {
    if (!querier_) return nullptr;
    auto* op_registry = static_cast<IOperatorRegistry*>(querier_->First(IID_OPERATOR_REGISTRY));
    if (op_registry) {
        const std::string key = category + "." + name;
        if (IOperator* op = op_registry->Create(key.c_str())) {
            return std::shared_ptr<IOperator>(op, [](IOperator* p) { delete p; });
        }
        if (IEquals(category, "builtin")) {
            if (IOperator* op = op_registry->Create(name.c_str())) {
                return std::shared_ptr<IOperator>(op, [](IOperator* p) { delete p; });
            }
        }
    }

    auto* bridge = static_cast<IBridge*>(querier_->First(IID_BRIDGE));
    if (bridge) {
        return bridge->FindOperator(category, name);
    }
    return nullptr;
}

// --- Build Database Query ---

// 从目标名称中提取表名（支持三段式 type.name.table）
static std::string ExtractTableName(const std::string& dest_name) {
    auto pos1 = dest_name.find('.');
    if (pos1 != std::string::npos) {
        auto pos2 = dest_name.find('.', pos1 + 1);
        if (pos2 != std::string::npos) {
            return dest_name.substr(pos2 + 1);  // 三段式
        }
        return dest_name.substr(pos1 + 1);  // 两段式
    }
    return dest_name;
}

// BuildQuery: 构建数据库查询语句
// 将 sql_part 中的第一个 FROM 子句替换为实际表名（支持多段式表名规范化）
// 子查询中的 FROM 保持原样，不做替换
static std::string BuildQuery(const std::string& source_name, const SqlStatement& stmt) {
    std::string sql = stmt.sql_part;
    std::string table = ExtractTableName(source_name);

    // 匹配第一个 FROM 子句（支持多段式表名如 catalog.db.table）
    // 使用不区分大小写，兼容 "from" / "From" 等写法。
    std::regex FROM_PATTERN(R"((\bFROM\s+)((?:[\w-]+\.)*[\w-]+))", std::regex_constants::icase);
    std::smatch m;
    if (std::regex_search(sql, m, FROM_PATTERN)) {
        // 只替换第一个匹配（主查询的 FROM），子查询不受影响
        sql = sql.substr(0, m.position()) +
              m[1].str() + table +
              sql.substr(m.position() + m.length());
    }

    return sql;
}

// --- 辅助：对 DataFrame 通道应用 WHERE 过滤 ---
static std::shared_ptr<DataFrameChannel> ApplyDataFrameFilter(
    IDataFrameChannel* src, const std::string& where_clause, uint64_t seq) {
    DataFrame data;
    if (src->Read(&data) != 0 || data.RowCount() == 0) return nullptr;

    if (data.Filter(where_clause.c_str()) != 0) return nullptr;

    auto filtered = std::make_shared<DataFrameChannel>("_filter", std::to_string(seq));
    filtered->Open();
    filtered->Write(&data);
    return filtered;
}

// --- 无算子：纯数据搬运 ---
int SchedulerPlugin::ExecuteTransfer(IChannel* source, IChannel* sink,
                                      const std::string& source_type,
                                      const std::string& sink_type,
                                      const SqlStatement& stmt, int64_t* rows_affected,
                                      std::string* error) {
    if (source_type == ChannelType::kDataFrame && sink_type == ChannelType::kDataFrame) {
        auto* src = dynamic_cast<IDataFrameChannel*>(source);
        auto* dst = dynamic_cast<IDataFrameChannel*>(sink);
        if (!src || !dst) return -1;

        // DataFrame + WHERE → 先过滤再复制
        if (!stmt.where_clause.empty()) {
            auto filtered = ApplyDataFrameFilter(src, stmt.where_clause, ++tmp_channel_seq_);
            if (!filtered) return -1;
            return ChannelAdapter::CopyDataFrame(filtered.get(), dst);
        }
        return ChannelAdapter::CopyDataFrame(src, dst);
    }

    if (source_type == ChannelType::kDataFrame && sink_type == ChannelType::kDatabase) {
        auto* src = dynamic_cast<IDataFrameChannel*>(source);
        auto* dst = dynamic_cast<IDatabaseChannel*>(sink);
        if (!src || !dst) return -1;
        std::string table = ExtractTableName(stmt.dest);

        // DataFrame + WHERE → 先过滤再写入
        if (!stmt.where_clause.empty()) {
            auto filtered = ApplyDataFrameFilter(src, stmt.where_clause, ++tmp_channel_seq_);
            if (!filtered) return -1;
            int64_t rows = ChannelAdapter::WriteFromDataFrame(filtered.get(), dst, table.c_str(), error);
            if (rows_affected) *rows_affected = rows;
            return (rows < 0) ? -1 : 0;
        }
        int64_t rows = ChannelAdapter::WriteFromDataFrame(src, dst, table.c_str(), error);
        if (rows_affected) *rows_affected = rows;
        return (rows < 0) ? -1 : 0;
    }

    if (source_type == ChannelType::kDataFrame && sink_type == ChannelType::kStream) {
        auto* src = dynamic_cast<IDataFrameChannel*>(source);
        auto* dst = dynamic_cast<IStreamChannel*>(sink);
        if (!src || !dst) return -1;

        IDataFrameChannel* payload_src = src;
        std::shared_ptr<DataFrameChannel> filtered;
        if (!stmt.where_clause.empty()) {
            filtered = ApplyDataFrameFilter(src, stmt.where_clause, ++tmp_channel_seq_);
            if (!filtered) return -1;
            payload_src = filtered.get();
        }

        DataFrame data;
        if (payload_src->Read(&data) != 0) return -1;
        if (rows_affected) *rows_affected = data.RowCount();

        if (data.RowCount() == 0) {
            dst->CloseStream();
            return 0;
        }

        auto batch = data.ToArrow();
        if (!batch) return -1;
        const int rc = dst->Put(std::move(batch), CurrentTimeMs());
        if (rc != 0) {
            if (error) *error = "write dataframe to stream failed, rc=" + std::to_string(rc);
            return -1;
        }
        dst->CloseStream();
        return 0;
    }

    if (source_type == ChannelType::kDatabase && sink_type == ChannelType::kDataFrame) {
        auto* src = dynamic_cast<IDatabaseChannel*>(source);
        auto* dst = dynamic_cast<IDataFrameChannel*>(sink);
        if (!src || !dst) return -1;
        std::string query = BuildQuery(stmt.source, stmt);
        return ChannelAdapter::ReadToDataFrame(src, query.c_str(), dst, error);
    }

    if (source_type == ChannelType::kDatabase && sink_type == ChannelType::kDatabase) {
        auto* src = dynamic_cast<IDatabaseChannel*>(source);
        auto* dst = dynamic_cast<IDatabaseChannel*>(sink);
        if (!src || !dst) return -1;

        auto tmp = std::make_shared<DataFrameChannel>("_adapter", std::to_string(++tmp_channel_seq_));
        tmp->Open();

        std::string query = BuildQuery(stmt.source, stmt);
        int rc = ChannelAdapter::ReadToDataFrame(src, query.c_str(), tmp.get(), error);
        if (rc != 0) return rc;

        std::string table = ExtractTableName(stmt.dest);
        int64_t rows = ChannelAdapter::WriteFromDataFrame(tmp.get(), dst, table.c_str(), error);
        if (rows_affected) *rows_affected = rows;
        return (rows < 0) ? -1 : 0;
    }

    if (error) *error = "unsupported transfer: " + source_type + " → " + sink_type;
    return -1;
}

// --- 有算子：自动适配通道类型 ---
int SchedulerPlugin::ExecuteWithOperator(IChannel* source, IChannel* sink,
                                          IOperator* op,
                                          const std::string& sink_type,
                                          const SqlStatement& stmt, int64_t* rows_affected,
                                          std::string* error) {
    std::vector<IOperator*> ops;
    std::vector<IChannel*> inputs;
    inputs.push_back(source);
    ops.push_back(op);
    return ExecuteWithOperatorChain(Span<IChannel*>(inputs), sink, ops, sink_type, stmt, rows_affected, error);
}

int SchedulerPlugin::ExecuteWithOperatorChain(Span<IChannel*> inputs, IChannel* sink,
                                              const std::vector<IOperator*>& ops,
                                              const std::string& sink_type,
                                              const SqlStatement& stmt, int64_t* rows_affected,
                                              std::string* error) {
    if (ops.empty() || inputs.empty()) return -1;

    Span<IChannel*> stage_inputs = inputs;
    std::shared_ptr<DataFrameChannel> tmp_in;
    std::vector<IChannel*> stage_input_holder;
    std::vector<std::shared_ptr<DataFrameChannel>> stage_buffers;

    // 单源算子路径仍保留 Database/DataFrame 自动适配；多源在 HandleExecute 限制为 dataframe.*
    if (stage_inputs.size == 1) {
        IChannel* single = stage_inputs[0];
        const std::string source_type(single->Type());

        if (source_type == ChannelType::kDatabase) {
            auto* db_src = dynamic_cast<IDatabaseChannel*>(single);
            if (!db_src) return -1;

            tmp_in = std::make_shared<DataFrameChannel>("_adapter", std::to_string(++tmp_channel_seq_));
            tmp_in->Open();

            std::string query = BuildQuery(stmt.source, stmt);
            int rc = ChannelAdapter::ReadToDataFrame(db_src, query.c_str(), tmp_in.get(), error);
            if (rc != 0) return rc;

            stage_input_holder.clear();
            stage_input_holder.push_back(tmp_in.get());
            stage_inputs = Span<IChannel*>(stage_input_holder);
        } else if (source_type == ChannelType::kDataFrame && !stmt.where_clause.empty()) {
            auto* df_src = dynamic_cast<IDataFrameChannel*>(single);
            if (!df_src) return -1;

            tmp_in = ApplyDataFrameFilter(df_src, stmt.where_clause, ++tmp_channel_seq_);
            if (!tmp_in) return -1;

            stage_input_holder.clear();
            stage_input_holder.push_back(tmp_in.get());
            stage_inputs = Span<IChannel*>(stage_input_holder);
        }
    } else {
        for (size_t i = 0; i < stage_inputs.size; ++i) {
            auto* df = dynamic_cast<IDataFrameChannel*>(stage_inputs[i]);
            if (!df) {
                if (error) *error = "multi-source operator input must be dataframe channel";
                return -1;
            }
        }
    }

    for (size_t i = 0; i < ops.size(); ++i) {
        std::shared_ptr<DataFrameChannel> stage_out;
        IChannel* actual_sink = nullptr;
        if (i + 1 == ops.size()) {
            if (sink_type == ChannelType::kDatabase) {
                stage_out = std::make_shared<DataFrameChannel>("_adapter", std::to_string(++tmp_channel_seq_));
                stage_out->Open();
                actual_sink = stage_out.get();
            } else {
                actual_sink = sink;
            }
        } else {
            stage_out = std::make_shared<DataFrameChannel>("_pipe", std::to_string(++tmp_channel_seq_));
            stage_out->Open();
            actual_sink = stage_out.get();
        }

        if (ops[i]->Work(stage_inputs, actual_sink) != 0) {
            if (error && error->empty()) {
                std::string op_error = ops[i]->LastError();
                if (!op_error.empty()) {
                    *error = op_error;
                } else {
                    *error = "operator " + ops[i]->Category() + "." + ops[i]->Name() + " execution failed";
                }
            }
            return -1;
        }

        if (stage_out) {
            stage_buffers.push_back(stage_out);
            stage_input_holder.clear();
            stage_input_holder.push_back(stage_out.get());
            stage_inputs = Span<IChannel*>(stage_input_holder);
        }
    }

    if (sink_type == ChannelType::kDatabase) {
        auto* db_sink = dynamic_cast<IDatabaseChannel*>(sink);
        if (!db_sink) return -1;
        if (stage_buffers.empty()) return -1;

        std::string table = ExtractTableName(stmt.dest);
        int64_t written_rows = ChannelAdapter::WriteFromDataFrame(stage_buffers.back().get(), db_sink, table.c_str(), error);
        if (written_rows < 0) return -1;

        if (rows_affected) *rows_affected = written_rows;
    }

    return 0;
}

std::string SchedulerPlugin::NextStreamTaskId() {
    const uint64_t seq = stream_task_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    std::ostringstream oss;
    oss << "stream_task_" << CurrentTimeMs() << "_" << seq;
    return oss.str();
}

std::string SchedulerPlugin::QueryStreamChannelRole(const std::string& type, const std::string& name) {
    auto* stream_manager = querier_ ? static_cast<IStreamManager*>(querier_->First(IID_STREAM_MANAGER)) : nullptr;
    if (!stream_manager) return "both";
    std::string role = "both";
    const std::string expect_type = ToLowerAscii(type);
    stream_manager->QueryChannels([&](const std::string& ch_type,
                                      const std::string& ch_name,
                                      const std::string& option,
                                      const std::string&) {
        if (ToLowerAscii(ch_type) != expect_type || ch_name != name) return;
        role = ReadRoleFromOption(option);
    });
    return role;
}

int32_t SchedulerPlugin::ResolveSourceBindings(const SqlStatement& stmt,
                                               SourceResolveResult* out,
                                               std::string* err_rsp) {
    if (!out) {
        if (err_rsp) *err_rsp = MakeErrorJson("source resolve target is null");
        return error::INTERNAL_ERROR;
    }
    out->channels.clear();
    out->channel_holders.clear();
    out->stream_channels.clear();
    out->source_keys.clear();
    out->resolved_sources.clear();
    out->source_expand_rule = "explicit";
    out->has_stream_source = false;
    out->has_non_stream_source = false;
    if (err_rsp) err_rsp->clear();

    auto fail = [&](int32_t status,
                    const std::string& message,
                    const std::string& error_code = "") -> int32_t {
        if (err_rsp) {
            if (error_code.empty()) {
                *err_rsp = MakeErrorJson(message);
            } else {
                *err_rsp = MakeExecutionErrorJson(message, error_code, "source_resolve");
            }
        }
        return status;
    };

    if (stmt.sources.empty()) {
        return fail(error::BAD_REQUEST, "source channel not found");
    }

    for (const auto& source_ref : stmt.sources) {
        ParsedChannelRef ref;
        std::string parse_err;
        if (!ParseChannelRef(source_ref, &ref, &parse_err)) {
            return fail(error::BAD_REQUEST, parse_err, "STREAM_HUB_SELECTOR_INVALID");
        }

        std::shared_ptr<IChannel> source_owner;
        IChannel* source_ch = FindChannel(ref.base, &source_owner);
        if (!source_ch) {
            return fail(IsDataFrameRef(ref.base) ? error::NOT_FOUND : error::BAD_REQUEST,
                        "source channel not found: " + ref.base);
        }
        if (source_owner) {
            out->channel_holders.push_back(source_owner);
        }

        const std::string source_type = source_ch->Type() ? source_ch->Type() : "";
        if (source_type == ChannelType::kBlockStream) {
            return fail(error::BAD_REQUEST,
                        "block stream source is not implemented in current release",
                        "BLOCK_STREAM_NOT_IMPLEMENTED");
        }

        if (source_type != ChannelType::kStream) {
            if (ref.has_selector) {
                return fail(error::BAD_REQUEST,
                            "channel selector is only supported on stream_hub source: " + source_ref,
                            "STREAM_HUB_SELECTOR_INVALID");
            }
            out->has_non_stream_source = true;
            out->channels.push_back(source_ch);
            out->resolved_sources.push_back(ref.base);
            continue;
        }

        out->has_stream_source = true;
        auto* stream_ch = dynamic_cast<IStreamChannel*>(source_ch);
        if (!stream_ch) {
            return fail(error::BAD_REQUEST,
                        "source channel cast to IStreamChannel failed: " + ref.base);
        }
        const std::string source_role = QueryStreamChannelRole(stream_ch->Category(), stream_ch->Name());
        if (!IsSourceRoleAllowed(source_role)) {
            return fail(error::BAD_REQUEST,
                        "stream channel role does not allow source: " +
                            std::string(stream_ch->Category()) + "." + stream_ch->Name(),
                        "STREAM_CHANNEL_ROLE_MISMATCH");
        }

        if (stream_ch->IsHubChannel()) {
            if (IEquals(stream_ch->HubModeHint() ? stream_ch->HubModeHint() : "", "merge")) {
                if (ref.has_selector) {
                    return fail(error::BAD_REQUEST,
                                "stream_hub(merge) does not allow selector: " + source_ref,
                                "STREAM_HUB_SELECTOR_NOT_ALLOWED_MERGE");
                }
                out->channels.push_back(stream_ch);
                out->stream_channels.push_back(MakeStreamOwner(stream_ch, source_owner));
                out->source_keys.push_back(MakeStreamChannelKey(stream_ch->Category(), stream_ch->Name()));
                out->resolved_sources.push_back(ref.base);
                continue;
            }

            const size_t partition_count = stream_ch->HubPartitionCount();
            if (partition_count == 0) {
                return fail(error::BAD_REQUEST,
                            "stream_hub(split) has no derived partitions: " + ref.base,
                            "STREAM_HUB_SELECTOR_INVALID");
            }
            if (partition_count > max_resolved_sources_) {
                return fail(error::BAD_REQUEST,
                            "resolved sources exceed max_resolved_sources: " +
                                std::to_string(partition_count) + " > " + std::to_string(max_resolved_sources_),
                            "STREAM_HUB_SELECTOR_OUT_OF_RANGE");
            }

            if (!ref.has_selector || ref.wildcard_selector) {
                if (!ref.has_selector) {
                    out->source_expand_rule = "auto_wildcard";
                }
                for (size_t i = 0; i < partition_count; ++i) {
                    auto partition = stream_ch->HubPartition(i);
                    if (!partition) {
                        return fail(error::BAD_REQUEST,
                                    "stream_hub partition resolve failed: " + ref.base,
                                    "STREAM_HUB_SELECTOR_INVALID");
                    }
                    out->channels.push_back(partition.get());
                    out->stream_channels.push_back(partition);
                    out->source_keys.push_back(
                        MakeStreamChannelKey(partition->Category(), partition->Name()));
                    out->resolved_sources.push_back(
                        ref.base + "[" + std::to_string(i) + "]");
                }
                continue;
            }

            const int idx = ref.selector_index;
            if (idx < 0 || static_cast<size_t>(idx) >= partition_count) {
                return fail(error::BAD_REQUEST,
                            "stream_hub selector out of range: " + source_ref,
                            "STREAM_HUB_SELECTOR_OUT_OF_RANGE");
            }
            auto partition = stream_ch->HubPartition(static_cast<size_t>(idx));
            if (!partition) {
                return fail(error::BAD_REQUEST,
                            "stream_hub partition resolve failed: " + source_ref,
                            "STREAM_HUB_SELECTOR_INVALID");
            }
            out->channels.push_back(partition.get());
            out->stream_channels.push_back(partition);
            out->source_keys.push_back(
                MakeStreamChannelKey(partition->Category(), partition->Name()));
            out->resolved_sources.push_back(ref.base + "[" + std::to_string(idx) + "]");
            continue;
        }

        if (ref.has_selector) {
            return fail(error::BAD_REQUEST,
                        "channel selector is only supported on stream_hub source: " + source_ref,
                        "STREAM_HUB_SELECTOR_INVALID");
        }
        out->channels.push_back(stream_ch);
        out->stream_channels.push_back(MakeStreamOwner(stream_ch, source_owner));
        out->source_keys.push_back(MakeStreamChannelKey(stream_ch->Category(), stream_ch->Name()));
        out->resolved_sources.push_back(ref.base);
    }

    return error::OK;
}

int32_t SchedulerPlugin::ResolveStreamSink(
    const SqlStatement& stmt,
    SinkBinding* binding,
    std::string* err_out) {
    if (!binding) {
        if (err_out) *err_out = "invalid sink binding target";
        return error::BAD_REQUEST;
    }
    binding->sink_channel.reset();
    binding->sink_type.clear();
    binding->db_type.clear();
    binding->db_name.clear();
    binding->table_name.clear();

    if (stmt.dest.empty()) {
        if (err_out) *err_out = "stream task requires INTO destination";
        return error::BAD_REQUEST;
    }

    ParsedChannelRef dest_ref;
    std::string dest_parse_err;
    if (!ParseChannelRef(stmt.dest, &dest_ref, &dest_parse_err)) {
        if (err_out) *err_out = dest_parse_err;
        return error::BAD_REQUEST;
    }
    if (dest_ref.has_selector && IsStreamRef(dest_ref.base)) {
        if (err_out) {
            *err_out = MakeExecutionErrorJson(
                "INTO stream selector is not allowed: " + stmt.dest,
                "STREAM_HUB_SELECTOR_NOT_ALLOWED_INTO",
                "sink_resolve");
        }
        return error::BAD_REQUEST;
    }

    if (IsStreamRef(dest_ref.base)) {
        std::shared_ptr<IChannel> sink_owner;
        IChannel* sink_raw = FindChannel(dest_ref.base, &sink_owner);
        auto* matched = dynamic_cast<IStreamChannel*>(sink_raw);
        if (!matched) {
            if (err_out) *err_out = "stream sink not found: " + dest_ref.base;
            return error::NOT_FOUND;
        }
        const std::string sink_role = QueryStreamChannelRole(matched->Category(), matched->Name());
        if (!IsSinkRoleAllowed(sink_role)) {
            if (err_out) {
                *err_out = MakeExecutionErrorJson(
                    "stream channel role does not allow sink: " +
                        std::string(matched->Category()) + "." + matched->Name(),
                    "STREAM_CHANNEL_ROLE_MISMATCH",
                    "sink_resolve");
            }
            return error::BAD_REQUEST;
        }

        auto output = MakeStreamOwner(matched, sink_owner);
        if (output && !output->IsOpened()) {
            (void)output->Open();
        }
        binding->sink_channel = std::static_pointer_cast<IChannel>(output);
        binding->sink_type = ChannelType::kStream;
        return error::OK;
    }

    if (dest_ref.has_selector) {
        if (err_out) *err_out = "selector is only supported for stream source";
        return error::BAD_REQUEST;
    }

    if (IsDataFrameRef(dest_ref.base)) {
        auto* ch_registry = querier_
            ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY))
            : nullptr;
        if (!ch_registry) {
            if (err_out) *err_out = "channel registry unavailable";
            return error::UNAVAILABLE;
        }

        const std::string df_name = DataFrameNamePart(dest_ref.base);
        std::shared_ptr<IChannel> df_holder = ch_registry->Get(df_name.c_str());
        if (!df_holder) {
            auto created = std::make_shared<DataFrameChannel>("dataframe", df_name);
            (void)created->Open();
            if (ch_registry->Register(df_name.c_str(), std::static_pointer_cast<IChannel>(created)) != 0) {
                // 处理并发注册：重查一次
                df_holder = ch_registry->Get(df_name.c_str());
                if (!df_holder) {
                    if (err_out) *err_out = "register dataframe sink failed: " + dest_ref.base;
                    return error::INTERNAL_ERROR;
                }
            } else {
                df_holder = std::static_pointer_cast<IChannel>(created);
            }
        }

        auto appendable = std::dynamic_pointer_cast<IAppendableDataFrameChannel>(df_holder);
        if (!appendable) {
            if (err_out) *err_out = "dataframe sink is not appendable: " + dest_ref.base;
            return error::BAD_REQUEST;
        }
        if (!appendable->IsOpened()) {
            (void)appendable->Open();
        }

        binding->sink_channel = std::static_pointer_cast<IChannel>(appendable);
        binding->sink_type = ChannelType::kDataFrame;
        return error::OK;
    }

    std::string db_type;
    std::string db_name;
    std::string table_from_dest;
    if (!ParseDatabaseDestination(dest_ref.base, &db_type, &db_name, &table_from_dest)) {
        if (err_out) *err_out = "invalid INTO destination: " + dest_ref.base +
            ", expected stream.<name>, dataframe.<name>, or <db_type>.<db_name>[.<table>]";
        return error::BAD_REQUEST;
    }

    std::shared_ptr<IChannel> existing_owner;
    if (IChannel* existing = FindChannel(dest_ref.base, &existing_owner); existing) {
        if (!existing->IsOpened()) {
            (void)existing->Open();
        }
        binding->sink_channel = existing_owner ? existing_owner : MakeNonOwningChannelHolder(existing);
        binding->sink_type = existing->Type() ? existing->Type() : "";
        if (binding->sink_type == ChannelType::kDatabase) {
            binding->db_type = db_type;
            binding->db_name = db_name;
            binding->table_name = table_from_dest;
        }
        return error::OK;
    }

    auto* db_factory = querier_
        ? static_cast<IDatabaseFactory*>(querier_->First(IID_DATABASE_FACTORY))
        : nullptr;
    if (!db_factory) {
        if (err_out) *err_out = "database factory unavailable";
        return error::UNAVAILABLE;
    }

    IDatabaseChannel* db_raw = db_factory->Get(db_type.c_str(), db_name.c_str());
    if (!db_raw) {
        if (err_out) *err_out = "database channel not found: " + db_type + "." + db_name;
        return error::NOT_FOUND;
    }

    auto db_sink = std::shared_ptr<IDatabaseChannel>(db_raw, [](IDatabaseChannel*) {});
    if (!db_sink->IsOpened()) {
        (void)db_sink->Open();
    }

    binding->sink_channel = std::static_pointer_cast<IChannel>(db_sink);
    binding->sink_type = ChannelType::kDatabase;
    binding->db_type = db_type;
    binding->db_name = db_name;
    binding->table_name = table_from_dest;
    return error::OK;
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

void SchedulerPlugin::ReleaseStreamTaskLeases(const std::string& runtime_task_id) {
    if (runtime_task_id.empty()) return;
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
        ReleaseStreamTaskLeases(task_id);
    }
    if (!to_release.empty()) {
        std::lock_guard<std::mutex> lock(stream_group_nodes_mu_);
        for (const auto& task_id : to_release) {
            stream_group_node_owners_.erase(task_id);
        }
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

std::vector<BroadcastHubSnapshot> SchedulerPlugin::QueryGroupShareSetSnapshots(
    const std::string& group_runtime_task_id) {
    std::vector<BroadcastHubSnapshot> out;
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
        BroadcastHubSnapshot snap;
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

    std::vector<StreamGroupShareSetRuntime> share_sets;
    {
        std::lock_guard<std::mutex> lock(stream_group_share_sets_mu_);
        auto it = stream_group_share_sets_.find(group_runtime_task_id);
        if (it != stream_group_share_sets_.end()) {
            share_sets = std::move(it->second);
            stream_group_share_sets_.erase(it);
        }
    }

    std::vector<BroadcastHubSnapshot> final_snapshots;
    final_snapshots.reserve(share_sets.size());
    for (auto& ss : share_sets) {
        if (ss.hub) {
            final_snapshots.push_back(ss.hub->Snapshot());
            ss.hub->RequestStop();
            ss.hub->Join();
            final_snapshots.back() = ss.hub->Snapshot();
        } else {
            BroadcastHubSnapshot snap;
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

int32_t SchedulerPlugin::ExecuteStreamTask(const SqlStatement& stmt,
                                           std::string& rsp,
                                           const std::string& lease_owner_id,
                                           bool skip_lease_acquire) {
    if (!querier_) {
        rsp = MakeErrorJson("querier not initialized");
        return error::INTERNAL_ERROR;
    }

    if (stmt.dest.empty()) {
        rsp = MakeErrorJson("stream task requires INTO destination");
        return error::BAD_REQUEST;
    }
    if (!IsQualifiedDestination(stmt.dest)) {
        rsp = MakeErrorJson("invalid INTO destination: " + stmt.dest);
        return error::BAD_REQUEST;
    }

    std::vector<OperatorRef> parsed_ops = stmt.operators;
    if (parsed_ops.empty() && !stmt.op_category.empty() && !stmt.op_name.empty()) {
        parsed_ops.push_back({stmt.op_category, stmt.op_name});
    }
    if (parsed_ops.empty()) {
        rsp = MakeErrorJson("stream task requires USING stream operator");
        return error::BAD_REQUEST;
    }
    if (parsed_ops.size() != 1) {
        rsp = MakeErrorJson("stream task currently supports single USING operator");
        return error::BAD_REQUEST;
    }
    const OperatorRef& op_ref = parsed_ops[0];

    auto* catalog = static_cast<IOperatorCatalog*>(querier_->First(IID_OPERATOR_CATALOG));
    if (!catalog) {
        rsp = MakeErrorJson("operator catalog unavailable");
        return error::UNAVAILABLE;
    }
    OperatorStatus status = catalog->QueryStatus(op_ref.category, op_ref.name);
    if (status == OperatorStatus::kNotFound) {
        rsp = MakeErrorJson("operator not found: " + op_ref.category + "." + op_ref.name);
        return error::NOT_FOUND;
    }
    if (status == OperatorStatus::kDeactivated) {
        rsp = MakeErrorJson("operator is deactivated: " + op_ref.category + "." + op_ref.name);
        return error::CONFLICT;
    }

    SourceResolveResult source_resolved;
    std::string source_err_rsp;
    const int32_t source_rc = ResolveSourceBindings(stmt, &source_resolved, &source_err_rsp);
    if (source_rc != error::OK) {
        rsp = source_err_rsp.empty() ? MakeErrorJson("source resolve failed") : source_err_rsp;
        return source_rc;
    }
    if (!source_resolved.has_stream_source || source_resolved.has_non_stream_source) {
        rsp = MakeErrorJson("stream task requires stream source only");
        return error::BAD_REQUEST;
    }
    std::vector<std::shared_ptr<IStreamChannel>> source_channels = source_resolved.stream_channels;
    std::vector<std::string> source_keys = source_resolved.source_keys;
    if (source_channels.empty()) {
        rsp = MakeErrorJson("source channel not found");
        return error::BAD_REQUEST;
    }

    const std::unordered_map<std::string, std::string> with_params =
        !stmt.operator_with_params.empty()
            ? stmt.operator_with_params[0]
            : stmt.with_params;
    if (with_params.find("sink_table") != with_params.end()) {
        rsp = MakeErrorJson("sink_table is not supported for stream tasks; use INTO <db_type>.<db_name>.<table>");
        return error::BAD_REQUEST;
    }

    SinkBinding sink_binding;
    std::string sink_error;
    const int32_t sink_rc = ResolveStreamSink(stmt, &sink_binding, &sink_error);
    if (sink_rc != error::OK) {
        if (!sink_error.empty() && sink_error.front() == '{') {
            rsp = sink_error;
        } else {
            rsp = MakeErrorJson(sink_error);
        }
        return sink_rc;
    }
    auto output = sink_binding.sink_channel;
    if (!output) {
        rsp = MakeErrorJson("resolve stream sink failed: output channel is null");
        return error::INTERNAL_ERROR;
    }
    StreamSinkContext sink_ctx;
    sink_ctx.sink_channel = output.get();
    sink_ctx.sink_type = sink_binding.sink_type;
    sink_ctx.into_raw = stmt.dest;
    sink_ctx.db_type = sink_binding.db_type;
    sink_ctx.db_name = sink_binding.db_name;
    sink_ctx.table_name = sink_binding.table_name;

    const std::string task_id = NextStreamTaskId();
    std::vector<std::string> sink_keys;
    if (sink_ctx.sink_type == ChannelType::kStream) {
        auto* sink_stream = dynamic_cast<IStreamChannel*>(output.get());
        if (sink_stream) {
            sink_keys.push_back(MakeStreamChannelKey(sink_stream->Category(), sink_stream->Name()));
        }
    }

    bool release_lease_on_fail = !skip_lease_acquire;
    std::unique_ptr<void, std::function<void(void*)>> lease_guard(
        nullptr, [](void*) {});
    if (!skip_lease_acquire) {
        SweepFinishedTaskLeases();
        std::vector<std::string> lease_keys;
        lease_keys.reserve(source_keys.size() + sink_keys.size());
        lease_keys.insert(lease_keys.end(), source_keys.begin(), source_keys.end());
        lease_keys.insert(lease_keys.end(), sink_keys.begin(), sink_keys.end());

        std::unordered_map<std::string, uint64_t> version_snapshot;
        CaptureStreamChannelVersionSnapshot(lease_keys, &version_snapshot);

        std::string conflict_key;
        std::string version_conflict_key;
        bool blocked_by_mutation = false;
        const int lease_rc = TryAcquireStreamTaskLeases(task_id,
                                                        source_keys,
                                                        sink_keys,
                                                        &conflict_key,
                                                        &blocked_by_mutation,
                                                        lease_owner_id,
                                                        &version_snapshot,
                                                        &version_conflict_key);
        if (lease_rc != 0) {
            if (lease_rc == EBUSY) {
                if (blocked_by_mutation) {
                    rsp = MakeExecutionErrorJson(
                        "stream channel is being modified: " + conflict_key,
                        "STREAM_CHANNEL_MUTATING",
                        "lease");
                    return error::CONFLICT;
                }
                rsp = MakeExecutionErrorJson(
                    "stream source is in use: " + conflict_key,
                    "STREAM_SOURCE_IN_USE",
                    "lease");
                return error::CONFLICT;
            }
            if (lease_rc == EAGAIN) {
                rsp = MakeExecutionErrorJson(
                    "stream channel changed during execute prepare: " + version_conflict_key,
                    "STREAM_CHANNEL_VERSION_CHANGED",
                    "lease");
                return error::CONFLICT;
            }
            rsp = MakeExecutionErrorJson(
                "stream channel lease acquire failed",
                "STREAM_LEASE_FAILED",
                "lease");
            return MapStreamManagerErrorToStatus(lease_rc);
        }
        lease_guard = std::unique_ptr<void, std::function<void(void*)>>(
            reinterpret_cast<void*>(1),
            [this, task_id, &release_lease_on_fail](void*) {
                if (release_lease_on_fail) {
                    ReleaseStreamTaskLeases(task_id);
                }
            });
    }

    std::shared_ptr<FanInStreamChannel> fanin;
    std::shared_ptr<IStreamChannel> source = source_channels[0];
    if (source_channels.size() > 1) {
        for (size_t i = 0; i < source_channels.size(); ++i) {
            const auto& sc = source_channels[i];
            StreamChannelCapabilities caps = sc ? sc->Capabilities() : StreamChannelCapabilities{};
            if (!sc || !caps.semantics.supports_timeout_poll || !caps.concurrency.lock_free_poll) {
                const std::string src_name = (sc ? (std::string(sc->Category()) + "." + sc->Name())
                                                 : source_keys[i]);
                rsp = MakeExecutionErrorJson(
                    "stream fanin capability mismatch: source=" + src_name +
                    ", reason=source must support timeout poll and lock-free poll",
                    "STREAM_FANIN_CAPABILITY_MISMATCH",
                    "fanin");
                return error::BAD_REQUEST;
            }
        }
        fanin = std::make_shared<FanInStreamChannel>(
            "fanin", task_id + ".fanin", source_channels);
        source = fanin;
    }

    if (!stmt.where_clause.empty()) {
        std::vector<std::string> unsupported;
        const int filter_rc = source->SetFilter(stmt.where_clause.c_str(), &unsupported);
        if (filter_rc != 0) {
            rsp = MakeErrorJson("stream source SetFilter failed");
            return error::BAD_REQUEST;
        }
        if (!unsupported.empty()) {
            std::ostringstream oss;
            oss << "WHERE pushdown not fully supported:";
            for (size_t i = 0; i < unsupported.size(); ++i) {
                oss << (i == 0 ? " " : ", ") << unsupported[i];
            }
            rsp = MakeErrorJson(oss.str());
            return error::BAD_REQUEST;
        }
    }

    auto first_holder = CreateOperator(op_ref.category, op_ref.name);
    if (!first_holder) {
        rsp = MakeErrorJson("operator create failed: " + op_ref.category + "." + op_ref.name);
        return error::NOT_FOUND;
    }
    auto first_stream_op = std::dynamic_pointer_cast<IStreamOperator>(first_holder);
    if (!first_stream_op) {
        rsp = MakeErrorJson("operator is not stream operator: " + op_ref.category + "." + op_ref.name);
        return error::BAD_REQUEST;
    }

    ParallelStrategy strategy = first_stream_op->GetParallelStrategy();
    int parallelism = std::max(1, first_stream_op->GetParallelism());
    if (strategy == ParallelStrategy::NONE) {
        parallelism = 1;
    }
    if (parallelism < 1) parallelism = 1;

    StreamChannelCapabilities source_caps = source->Capabilities();
    StreamChannelCapabilities sink_caps;
    if (sink_ctx.sink_type == ChannelType::kStream) {
        auto* sink_stream = dynamic_cast<IStreamChannel*>(output.get());
        if (!sink_stream) {
            rsp = MakeErrorJson("stream sink cast to IStreamChannel failed");
            return error::BAD_REQUEST;
        }
        sink_caps = sink_stream->Capabilities();
    } else {
        sink_caps.channel_type = sink_ctx.sink_type;
        sink_caps.concurrency.put_mode = ProducerMode::SINGLE;
        sink_caps.concurrency.poll_mode = ConsumerMode::SINGLE;
        sink_caps.concurrency.max_producers = 1;
        sink_caps.concurrency.max_consumers = 1;
        sink_caps.concurrency.lock_free_put = false;
        sink_caps.concurrency.lock_free_poll = false;
        sink_caps.concurrency.cancel_wakeup_guaranteed = false;
    }

    if (strategy == ParallelStrategy::STATELESS && parallelism > 1) {
        const bool poll_mode_ok = source_caps.concurrency.poll_mode == ConsumerMode::MULTI;
        const bool consumers_ok = source_caps.concurrency.max_consumers == 0 ||
                                  source_caps.concurrency.max_consumers >= static_cast<uint32_t>(parallelism);
        if (!poll_mode_ok || !consumers_ok) {
            rsp = MakeCapabilityMismatchJson(
                "stream source capability mismatch: strategy=STATELESS, parallelism=" + std::to_string(parallelism) +
                ", required.poll_mode=MULTI, actual.poll_mode=" + std::string(ConsumerModeName(source_caps.concurrency.poll_mode)) +
                ", actual.max_consumers=" + std::to_string(source_caps.concurrency.max_consumers),
                "STREAM_SOURCE_CAPABILITY_MISMATCH",
                &source_caps,
                &sink_caps);
            return error::BAD_REQUEST;
        }
    }

    if (parallelism > 1) {
        const bool put_mode_ok = sink_caps.concurrency.put_mode == ProducerMode::MULTI;
        const bool producers_ok = sink_caps.concurrency.max_producers == 0 ||
                                  sink_caps.concurrency.max_producers >= static_cast<uint32_t>(parallelism);
        if (!put_mode_ok || !producers_ok) {
            rsp = MakeCapabilityMismatchJson(
                "stream sink capability mismatch: strategy=" + std::to_string(static_cast<int>(strategy)) +
                ", parallelism=" + std::to_string(parallelism) +
                ", required.put_mode=MULTI, actual.put_mode=" + std::string(ProducerModeName(sink_caps.concurrency.put_mode)) +
                ", actual.max_producers=" + std::to_string(sink_caps.concurrency.max_producers),
                "STREAM_SINK_CAPABILITY_MISMATCH",
                &source_caps,
                &sink_caps);
            return error::BAD_REQUEST;
        }
    }

    std::shared_ptr<FanOutStreamChannel> fanout;
    std::shared_ptr<SharedSourceState> shared_source_state;
    std::vector<std::shared_ptr<IStreamChannel>> input_ports;
    input_ports.reserve(static_cast<size_t>(parallelism));
    std::shared_ptr<IStreamChannel> open_target = source;

    if (strategy == ParallelStrategy::NONE) {
        input_ports.push_back(source);
    } else if (strategy == ParallelStrategy::STATELESS) {
        shared_source_state = std::make_shared<SharedSourceState>(source);
        for (int i = 0; i < parallelism; ++i) {
            input_ports.push_back(std::make_shared<StatelessSourceView>(shared_source_state, static_cast<uint32_t>(i)));
        }
    } else if (strategy == ParallelStrategy::KEYED) {
        RingStreamChannelOptions partition_opts;
        const size_t source_cap = source->Capacity();
        if (source_cap > 0) {
            partition_opts.ring_size = NextPowerOfTwo(std::max<size_t>(64, source_cap));
        }
        fanout = std::make_shared<FanOutStreamChannel>(
            "fanout",
            task_id + ".fanout",
            source,
            static_cast<size_t>(parallelism),
            FanOutMode::ROUTE_BY_PARTITION_ID,
            first_stream_op->GetPartitionSpec(),
            partition_opts);
        open_target = fanout;
        for (int i = 0; i < parallelism; ++i) {
            auto part = fanout->GetPartition(static_cast<size_t>(i));
            if (!part) {
                rsp = MakeErrorJson("fanout partition create failed");
                return error::INTERNAL_ERROR;
            }
            input_ports.push_back(std::make_shared<FanOutPartitionView>(fanout, part));
        }
    } else {
        rsp = MakeErrorJson("unsupported stream parallel strategy");
        return error::BAD_REQUEST;
    }

    if (input_ports.empty()) {
        rsp = MakeErrorJson("stream input ports build failed");
        return error::INTERNAL_ERROR;
    }

    const std::string with_params_json = MakeWithParamsJson(with_params);
    const std::shared_ptr<arrow::Schema> static_schema = source->GetOutputSchema();

    auto task = std::make_shared<StreamTask>(task_id, &stream_runtime_);

    for (size_t i = 0; i < input_ports.size(); ++i) {
        std::shared_ptr<IOperator> op_holder;
        if (i == 0) {
            op_holder = first_holder;
        } else {
            op_holder = CreateOperator(op_ref.category, op_ref.name);
        }
        if (!op_holder) {
            rsp = MakeErrorJson("operator create failed for shard");
            return error::INTERNAL_ERROR;
        }

        auto stream_op = std::dynamic_pointer_cast<IStreamOperator>(op_holder);
        if (!stream_op) {
            rsp = MakeErrorJson("stream operator cast failed for shard");
            return error::INTERNAL_ERROR;
        }

        int init_rc = stream_op->Init(with_params_json.c_str(), sink_ctx);
        if (init_rc != 0) {
            const std::string err = stream_op->LastError().empty()
                ? "stream operator Init failed"
                : stream_op->LastError();
            rsp = MakeErrorJson(err);
            return error::BAD_REQUEST;
        }

        if (static_schema) {
            int schema_rc = stream_op->OnSchemaReady(static_schema);
            if (schema_rc != 0) {
                const std::string err = stream_op->LastError().empty()
                    ? "stream operator OnSchemaReady failed"
                    : stream_op->LastError();
                rsp = MakeErrorJson(err);
                return error::BAD_REQUEST;
            }
        }

        task->AddShard(std::make_shared<ShardRunner>(
            static_cast<uint32_t>(i),
            input_ports[i],
            stream_op,
            output,
            task.get(),
            static_schema != nullptr));
    }
    task->PrepareForRun(static_cast<uint32_t>(input_ports.size()), CurrentTimeMs());
    task->SetSourceResolveMeta(source_resolved.resolved_sources, source_resolved.source_expand_rule);

    const int open_rc = open_target->Open();
    if (open_rc != 0) {
        rsp = MakeErrorJson("open stream source failed");
        return error::INTERNAL_ERROR;
    }

    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        stream_tasks_[task->Id()] = task;
    }
    for (const auto& shard : task->Shards()) {
        stream_runtime_.TrySchedule(shard);
    }
    if (!skip_lease_acquire) {
        release_lease_on_fail = false;
        lease_guard.reset();
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("status");
    w.String("submitted");
    w.Key("runtime_task_id");
    w.String(task->Id().c_str());
    w.Key("task_id");
    w.String(task->Id().c_str());
    w.Key("runtime_kind");
    w.String("single");
    w.Key("resolved_sources");
    w.StartArray();
    for (const auto& source_name : source_resolved.resolved_sources) {
        w.String(source_name.c_str());
    }
    w.EndArray();
    w.Key("source_expand_rule");
    w.String(source_resolved.source_expand_rule.c_str());
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t SchedulerPlugin::ClassifySqlTaskKind(const std::string& sql_text,
                                             std::string* task_kind,
                                             std::string* err_rsp) {
    if (!task_kind || !err_rsp) return error::INTERNAL_ERROR;
    task_kind->clear();
    err_rsp->clear();

    static constexpr size_t kMaxSqlLength = 64 * 1024;
    if (sql_text.size() > kMaxSqlLength) {
        *err_rsp = MakeErrorJson("SQL too long (max 64KB)");
        return error::BAD_REQUEST;
    }

    SqlParser parser;
    SqlStatement stmt = parser.Parse(sql_text);
    if (!stmt.error.empty()) {
        *err_rsp = MakeErrorJson(stmt.error);
        return error::BAD_REQUEST;
    }
    if (stmt.sources.empty() && !stmt.source.empty()) {
        stmt.sources.push_back(stmt.source);
    }
    if (stmt.sources.empty()) {
        *err_rsp = MakeErrorJson("source channel not found");
        return error::BAD_REQUEST;
    }

    SourceResolveResult source_resolved;
    const int32_t resolve_rc = ResolveSourceBindings(stmt, &source_resolved, err_rsp);
    if (resolve_rc != error::OK) {
        return resolve_rc;
    }

    if (source_resolved.has_stream_source && source_resolved.has_non_stream_source) {
        *err_rsp = MakeErrorJson("mixed stream and non-stream sources are not supported");
        return error::BAD_REQUEST;
    }

    *task_kind = source_resolved.has_stream_source ? "stream" : "batch";
    return error::OK;
}

int32_t SchedulerPlugin::HandleSqlClassify(const std::string&, const std::string& req_body, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req_body.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("sql") || !doc["sql"].IsString()) {
        rsp = MakeErrorJson("invalid request, expected {\"sql\":\"...\"}");
        return error::BAD_REQUEST;
    }

    std::string task_kind;
    std::string err_rsp;
    const int32_t rc = ClassifySqlTaskKind(doc["sql"].GetString(), &task_kind, &err_rsp);
    if (rc != error::OK) {
        rsp = err_rsp.empty() ? MakeErrorJson("sql classify failed") : err_rsp;
        return rc;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("task_kind");
    w.String(task_kind.c_str());
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t SchedulerPlugin::HandleStreamExecuteSingle(const rapidjson::Document& doc, std::string& rsp) {
    if (doc.HasMember("group_mode") ||
        doc.HasMember("dag") ||
        doc.HasMember("sql") ||
        doc.HasMember("sqls") ||
        doc.HasMember("share_set_ready_timeout_s")) {
        rsp = MakeExecutionErrorJson(
            "single execution accepts only sql_text and timeout_s",
            "STREAM_GROUP_SQL_TEXT_INVALID",
            "request");
        return error::BAD_REQUEST;
    }
    if (!doc.HasMember("sql_text") || !doc["sql_text"].IsString()) {
        rsp = MakeExecutionErrorJson(
            "invalid request, expected {\"sql_text\":\"...\"}",
            "STREAM_GROUP_SQL_TEXT_INVALID",
            "request");
        return error::BAD_REQUEST;
    }
    std::vector<std::string> sqls;
    SqlTextSplitError split_err;
    if (SplitSqlText(doc["sql_text"].GetString(), &sqls, &split_err) != 0) {
        std::string err = "invalid sql_text";
        if (!split_err.message.empty()) {
            err += ": " + split_err.message;
        }
        rsp = MakeExecutionErrorWithSqlIndexJson(
            err,
            "STREAM_GROUP_SQL_TEXT_INVALID",
            "request",
            split_err.statement_index);
        return error::BAD_REQUEST;
    }
    if (sqls.size() != 1) {
        rsp = MakeExecutionErrorJson(
            "single execution requires exactly one SQL statement",
            "STREAM_GROUP_SQL_TEXT_INVALID",
            "request");
        return error::BAD_REQUEST;
    }

    SqlParser parser;
    SqlStatement stmt = parser.Parse(sqls.front());
    if (!stmt.error.empty()) {
        rsp = MakeErrorJson(stmt.error);
        return error::BAD_REQUEST;
    }
    if (stmt.sources.empty() && !stmt.source.empty()) {
        stmt.sources.push_back(stmt.source);
    }
    if (stmt.sources.empty()) {
        rsp = MakeErrorJson("source channel not found");
        return error::BAD_REQUEST;
    }
    return ExecuteStreamTask(stmt, rsp);
}

int32_t SchedulerPlugin::HandleStreamExecute(const std::string&, const std::string& req_body, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req_body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        rsp = MakeErrorJson("invalid request body");
        return error::BAD_REQUEST;
    }
    if (doc.HasMember("task_id")) {
        rsp = MakeErrorJson("external task_id is not allowed");
        return error::BAD_REQUEST;
    }

    std::string execution_kind = "single";
    if (doc.HasMember("execution_kind")) {
        if (!doc["execution_kind"].IsString()) {
            rsp = MakeExecutionErrorJson(
                "execution_kind must be string",
                "STREAM_GROUP_SQL_TEXT_INVALID",
                "request");
            return error::BAD_REQUEST;
        }
        execution_kind = ToLowerAscii(doc["execution_kind"].GetString());
    }

    if (execution_kind == "single") {
        return HandleStreamExecuteSingle(doc, rsp);
    }
    if (execution_kind == "group") {
        return HandleStreamExecuteGroup(doc, rsp);
    }

    rsp = MakeExecutionErrorJson(
        "unsupported execution_kind: " + execution_kind,
        "STREAM_GROUP_SQL_TEXT_INVALID",
        "request");
    return error::BAD_REQUEST;
}

int32_t SchedulerPlugin::HandleStreamStop(const std::string&, const std::string& req, std::string& rsp) {
    SweepFinishedTaskLeases();
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("task_id") || !doc["task_id"].IsString()) {
        rsp = MakeErrorJson("invalid request, expected {\"task_id\":\"...\"}");
        return error::BAD_REQUEST;
    }
    const std::string task_id = doc["task_id"].GetString();
    {
        std::lock_guard<std::mutex> lock(stream_group_nodes_mu_);
        if (stream_group_node_owners_.find(task_id) != stream_group_node_owners_.end()) {
            rsp = MakeErrorJson("group node runtime_task_id is internal; use group task_id");
            return error::BAD_REQUEST;
        }
    }

    std::shared_ptr<StreamTaskGroup> group;
    {
        std::lock_guard<std::mutex> lock(stream_task_groups_mu_);
        auto it = stream_task_groups_.find(task_id);
        if (it != stream_task_groups_.end()) {
            group = it->second;
        }
    }
    if (group) {
        group->RequestStop();
        group->Join();
        StreamGroupSnapshot snapshot = group->Snapshot();
        const auto node_sources = QueryGroupNodeResolvedSources(task_id);
        const auto share_sets = QueryGroupShareSetSnapshots(task_id);
        CleanupGroupRuntimeResources(task_id, &snapshot);
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        WriteGroupSnapshotJson(&w, snapshot, &share_sets,
                               node_sources.empty() ? nullptr : &node_sources);
        rsp = buf.GetString();
        return error::OK;
    }

    std::shared_ptr<StreamTask> task;
    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        auto it = stream_tasks_.find(task_id);
        if (it == stream_tasks_.end()) {
            rsp = MakeErrorJson("stream task not found: " + task_id);
            return error::NOT_FOUND;
        }
        task = it->second;
    }

    task->RequestStop();
    task->Join();
    ReleaseStreamTaskLeases(task_id);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    WriteTaskSnapshotJson(&w, task->Snapshot());
    rsp = buf.GetString();
    return error::OK;
}

int32_t SchedulerPlugin::HandleStreamStatus(const std::string&, const std::string& req, std::string& rsp) {
    SweepFinishedTaskLeases();
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("task_id") || !doc["task_id"].IsString()) {
        rsp = MakeErrorJson("invalid request, expected {\"task_id\":\"...\"}");
        return error::BAD_REQUEST;
    }
    const std::string task_id = doc["task_id"].GetString();
    {
        std::lock_guard<std::mutex> lock(stream_group_nodes_mu_);
        if (stream_group_node_owners_.find(task_id) != stream_group_node_owners_.end()) {
            rsp = MakeErrorJson("group node runtime_task_id is internal; use group task_id");
            return error::BAD_REQUEST;
        }
    }

    std::shared_ptr<StreamTaskGroup> group;
    {
        std::lock_guard<std::mutex> lock(stream_task_groups_mu_);
        auto it = stream_task_groups_.find(task_id);
        if (it != stream_task_groups_.end()) {
            group = it->second;
        }
    }
    if (group) {
        StreamGroupSnapshot snapshot = group->Snapshot();
        const auto node_sources = QueryGroupNodeResolvedSources(task_id);
        const auto share_sets = QueryGroupShareSetSnapshots(task_id);
        if (IsTerminalStreamGroupStatus(snapshot.status)) {
            CleanupGroupRuntimeResources(task_id, &snapshot);
        }
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        WriteGroupSnapshotJson(&w, snapshot, &share_sets,
                               node_sources.empty() ? nullptr : &node_sources);
        rsp = buf.GetString();
        return error::OK;
    }

    std::shared_ptr<StreamTask> task;
    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        auto it = stream_tasks_.find(task_id);
        if (it == stream_tasks_.end()) {
            rsp = MakeErrorJson("stream task not found: " + task_id);
            return error::NOT_FOUND;
        }
        task = it->second;
    }

    TaskSnapshot snapshot = task->Snapshot();
    if (IsTerminalStreamTaskStatus(snapshot.status)) {
        ReleaseStreamTaskLeases(task_id);
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    WriteTaskSnapshotJson(&w, snapshot);
    rsp = buf.GetString();
    return error::OK;
}

int32_t SchedulerPlugin::HandleStreamList(const std::string&, const std::string&, std::string& rsp) {
    SweepFinishedTaskLeases();
    std::vector<TaskSnapshot> snapshots;
    std::vector<StreamGroupSnapshot> group_snapshots;
    std::vector<std::string> terminal_tasks;
    std::unordered_set<std::string> internal_node_ids;
    {
        std::lock_guard<std::mutex> lock(stream_group_nodes_mu_);
        internal_node_ids.reserve(stream_group_node_owners_.size());
        for (const auto& kv : stream_group_node_owners_) {
            internal_node_ids.insert(kv.first);
        }
    }
    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        snapshots.reserve(stream_tasks_.size());
        for (const auto& kv : stream_tasks_) {
            if (internal_node_ids.count(kv.first) > 0) continue;
            if (!kv.second) continue;
            TaskSnapshot s = kv.second->Snapshot();
            if (IsTerminalStreamTaskStatus(s.status)) {
                terminal_tasks.push_back(kv.first);
            }
            snapshots.push_back(std::move(s));
        }
    }
    {
        std::lock_guard<std::mutex> lock(stream_task_groups_mu_);
        group_snapshots.reserve(stream_task_groups_.size());
        for (const auto& kv : stream_task_groups_) {
            if (!kv.second) continue;
            StreamGroupSnapshot s = kv.second->Snapshot();
            if (IsTerminalStreamGroupStatus(s.status)) {
                for (const auto& node : s.nodes) {
                    terminal_tasks.push_back(node.runtime_task_id);
                }
            }
            group_snapshots.push_back(std::move(s));
        }
    }
    for (const auto& task_id : terminal_tasks) {
        ReleaseStreamTaskLeases(task_id);
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("tasks");
    w.StartArray();
    for (const auto& s : snapshots) {
        WriteTaskSnapshotJson(&w, s);
    }
    for (const auto& s : group_snapshots) {
        const auto node_sources = QueryGroupNodeResolvedSources(s.task_id);
        const auto share_sets = QueryGroupShareSetSnapshots(s.task_id);
        if (IsTerminalStreamGroupStatus(s.status)) {
            CleanupGroupRuntimeResources(s.task_id, &s);
        }
        WriteGroupSnapshotJson(&w, s, &share_sets, node_sources.empty() ? nullptr : &node_sources);
    }
    w.EndArray();
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

// --- HandleExecute ---
int32_t SchedulerPlugin::HandleExecute(const std::string&, const std::string& req_body, std::string& rsp) {
    auto* ch_registry = querier_ ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY)) : nullptr;
    rapidjson::Document doc;
    doc.Parse(req_body.c_str());
    if (doc.HasParseError() || !doc.HasMember("sql") || !doc["sql"].IsString()) {
        rsp = MakeErrorJson("invalid request, expected {\"sql\":\"...\"}");
        return error::BAD_REQUEST;
    }
    std::string sql_text = doc["sql"].GetString();

    static constexpr size_t kMaxSqlLength = 64 * 1024;
    if (sql_text.size() > kMaxSqlLength) {
        rsp = MakeErrorJson("SQL too long (max 64KB)");
        return error::BAD_REQUEST;
    }

    SqlParser parser;
    auto stmt = parser.Parse(sql_text);
    if (!stmt.error.empty()) {
        rsp = MakeErrorJson(stmt.error);
        return error::BAD_REQUEST;
    }

    if (stmt.sources.empty() && !stmt.source.empty()) {
        stmt.sources.push_back(stmt.source);
    }
    if (stmt.sources.empty()) {
        rsp = MakeErrorJson("source channel not found");
        return error::BAD_REQUEST;
    }

    SourceResolveResult source_resolved;
    std::string source_err_rsp;
    const int32_t source_rc = ResolveSourceBindings(stmt, &source_resolved, &source_err_rsp);
    if (source_rc != error::OK) {
        rsp = source_err_rsp.empty() ? MakeErrorJson("source resolve failed") : source_err_rsp;
        return source_rc;
    }
    if (source_resolved.has_stream_source && source_resolved.has_non_stream_source) {
        rsp = MakeErrorJson("mixed stream and non-stream sources are not supported");
        return error::BAD_REQUEST;
    }
    if (source_resolved.has_stream_source) {
        return ExecuteStreamTask(stmt, rsp);
    }

    std::vector<IChannel*> input_channels = source_resolved.channels;

    std::vector<std::shared_ptr<IOperator>> op_holders;
    std::vector<IOperator*> op_chain;
    std::vector<OperatorRef> parsed_ops = stmt.operators;
    if (parsed_ops.empty() && !stmt.op_category.empty() && !stmt.op_name.empty()) {
        parsed_ops.push_back({stmt.op_category, stmt.op_name});
    }
    if (!parsed_ops.empty()) {
        auto* catalog = querier_ ? static_cast<IOperatorCatalog*>(querier_->First(IID_OPERATOR_CATALOG)) : nullptr;
        if (!catalog) {
            rsp = MakeErrorJson("operator catalog unavailable");
            return error::UNAVAILABLE;
        }
        for (const auto& op_ref : parsed_ops) {
            OperatorStatus status = catalog->QueryStatus(op_ref.category, op_ref.name);
            if (status == OperatorStatus::kNotFound) {
                rsp = MakeErrorJson("operator not found: " + op_ref.category + "." + op_ref.name);
                return error::NOT_FOUND;
            }
            if (status == OperatorStatus::kDeactivated) {
                rsp = MakeErrorJson("operator is deactivated: " + op_ref.category + "." + op_ref.name);
                return error::CONFLICT;
            }
            auto holder = FindOperator(op_ref.category, op_ref.name);
            if (!holder) {
                rsp = MakeErrorJson("operator not found: " + op_ref.category + "." + op_ref.name);
                return error::NOT_FOUND;
            }
            op_chain.push_back(holder.get());
            op_holders.push_back(std::move(holder));
        }
    }

    try {
        if (!op_chain.empty()) {
            for (size_t i = 0; i < op_chain.size(); ++i) {
                const auto& params = (i < stmt.operator_with_params.size())
                    ? stmt.operator_with_params[i]
                    : (i == 0 ? stmt.with_params : std::unordered_map<std::string, std::string>{});
                for (const auto& kv : params) {
                    op_chain[i]->Configure(kv.first.c_str(), kv.second.c_str());
                }
            }
        }

        std::shared_ptr<DataFrameChannel> temp_sink;
        std::shared_ptr<IDataFrameChannel> named_df_sink;
        std::shared_ptr<IChannel> named_sink_holder;
        IChannel* sink = nullptr;

        if (!stmt.dest.empty()) {
            if (!IsQualifiedDestination(stmt.dest)) {
                rsp = MakeErrorJson("invalid INTO destination: " + stmt.dest +
                                    ", expected dataframe.<name> or <type>.<name>[.<table>]");
                return error::BAD_REQUEST;
            }
            if (IsDataFrameRef(stmt.dest)) {
                if (!ch_registry) {
                    rsp = MakeErrorJson("channel registry unavailable");
                    return error::INTERNAL_ERROR;
                }
                std::string df_name = DataFrameNamePart(stmt.dest);
                auto ch = std::make_shared<DataFrameChannel>("dataframe", df_name);
                ch->Open();
                named_df_sink = ch;
                sink = ch.get();
            } else {
                sink = FindChannel(stmt.dest, &named_sink_holder);
                if (!sink) {
                    rsp = MakeErrorJson("destination channel not found: " + stmt.dest);
                    return error::NOT_FOUND;
                }
            }
        } else {
            temp_sink = std::make_shared<DataFrameChannel>("_temp", "sink");
            temp_sink->Open();
            sink = temp_sink.get();
        }

        int rc = 0;
        int64_t affected_rows = 0;
        std::string exec_error;
        std::string sink_type(sink->Type());

        if (input_channels.size() > 1 && op_chain.empty()) {
            rsp = MakeErrorJson("multi-source FROM requires USING operator");
            return error::BAD_REQUEST;
        }
        if (input_channels.size() > 1) {
            for (const auto& source_name : stmt.sources) {
                if (!IsDataFrameRef(source_name)) {
                    rsp = MakeErrorJson("multi-source FROM only supports dataframe.* in Sprint 10");
                    return error::BAD_REQUEST;
                }
            }
            if (!stmt.where_clause.empty()) {
                rsp = MakeErrorJson("multi-source FROM does not support WHERE in Sprint 10");
                return error::BAD_REQUEST;
            }
        }

        if (op_chain.empty()) {
            if (input_channels.size() != 1) {
                rsp = MakeErrorJson("invalid source count");
                return error::BAD_REQUEST;
            }
            IChannel* source = input_channels[0];
            std::string source_type(source->Type());
            rc = ExecuteTransfer(source, sink, source_type, sink_type, stmt, &affected_rows, &exec_error);
        } else {
            rc = ExecuteWithOperatorChain(Span<IChannel*>(input_channels), sink, op_chain, sink_type, stmt,
                                          &affected_rows, &exec_error);
        }

        if (rc != 0) {
            std::string err = exec_error;
            if (err.empty() && !op_chain.empty()) err = op_chain.back()->LastError();
            if (err.empty()) err = "execution failed";
            std::string stage = ExtractStageFromExecutionError(err);
            if (stage.empty()) stage = "execute";
            rsp = MakeExecutionErrorJson(err, "OP_EXEC_FAIL", stage);
            return error::INTERNAL_ERROR;
        }

        // INTO dataframe.<name>：覆盖语义（已存在则先注销，再注册新结果）
        if (!stmt.dest.empty() && IsDataFrameRef(stmt.dest) && named_df_sink) {
            std::string df_name = DataFrameNamePart(stmt.dest);
            if (ch_registry->Get(df_name.c_str())) {
                (void)ch_registry->Unregister(df_name.c_str());
            }
            if (ch_registry->Register(df_name.c_str(), std::static_pointer_cast<IChannel>(named_df_sink)) != 0) {
                rsp = MakeErrorJson("failed to register dataframe channel: " + df_name);
                return error::INTERNAL_ERROR;
            }
            auto registered = ch_registry->Get(df_name.c_str());
            if (!registered) {
                rsp = MakeErrorJson("failed to fetch registered dataframe channel: " + df_name);
                return error::INTERNAL_ERROR;
            }
            auto* registered_df = dynamic_cast<IDataFrameChannel*>(registered.get());
            if (!registered_df) {
                rsp = MakeErrorJson("registered channel is not dataframe: " + df_name);
                return error::INTERNAL_ERROR;
            }
            sink = registered_df;
            sink_type = sink->Type();
        }

        auto* df_sink = dynamic_cast<IDataFrameChannel*>(sink);
        DataFrame result;
        std::string result_json = "[]";
        int64_t row_count = 0;
        if (df_sink && df_sink->Read(&result) == 0 && result.RowCount() > 0) {
            result_json = result.ToJson();
            row_count = result.RowCount();
        } else if (sink_type == ChannelType::kDatabase) {
            row_count = affected_rows;
        }

        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("status"); w.String("completed");
        w.Key("rows"); w.Int64(row_count);
        w.Key("result_row_count"); w.Int64(row_count);
        w.Key("result_target"); w.String(stmt.dest.c_str());
        w.Key("data"); w.RawValue(result_json.c_str(), result_json.size(), rapidjson::kArrayType);
        w.EndObject();
        rsp = buf.GetString();
        return error::OK;

    } catch (const std::exception& e) {
        std::string err = std::string("internal error: ") + e.what();
        LOG_ERROR("SchedulerPlugin::HandleExecute: exception: %s", err.c_str());
        rsp = MakeErrorJson(err);
        return error::INTERNAL_ERROR;
    } catch (...) {
        LOG_ERROR("SchedulerPlugin::HandleExecute: unknown exception");
        rsp = MakeErrorJson("internal error: unknown exception");
        return error::INTERNAL_ERROR;
    }
}

// --- HandleGetChannels ---
int32_t SchedulerPlugin::HandleGetChannels(const std::string&, const std::string&, std::string& rsp) {
    auto* ch_registry = querier_ ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY)) : nullptr;
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();

    // 内部通道表
    auto managed_snapshot = SnapshotManagedChannels();
    for (auto& [key, ch_ptr] : managed_snapshot) {
        auto* ch = ch_ptr.get();
        w.StartObject();
        w.Key("category"); w.String(ch->Category());
        w.Key("name"); w.String(ch->Name());
        w.Key("type"); w.String(ch->Type());
        w.Key("schema"); w.String(ch->Schema());
        w.EndObject();
    }

    // 具名 DataFrame 通道（CatalogPlugin 注册中心）
    if (ch_registry) {
        ch_registry->List([&w](const char* name, std::shared_ptr<IChannel> ch) {
            if (!name || !ch) return;
            w.StartObject();
            w.Key("category"); w.String(ch->Category());
            w.Key("name"); w.String(name);
            w.Key("type"); w.String(ch->Type());
            w.Key("schema"); w.String(ch->Schema());
            w.EndObject();
        });
    }

    // 静态注册的通道（通过 IQuerier）
    if (querier_) {
        querier_->Traverse(IID_CHANNEL, [&w](void* p) -> int {
            auto* ch = static_cast<IChannel*>(p);
            w.StartObject();
            w.Key("category"); w.String(ch->Category());
            w.Key("name"); w.String(ch->Name());
            w.Key("type"); w.String(ch->Type());
            w.Key("schema"); w.String(ch->Schema());
            w.EndObject();
            return 0;
        });

        // 数据库通道（通过 IDatabaseFactory）
        auto* factory = static_cast<IDatabaseFactory*>(querier_->First(IID_DATABASE_FACTORY));
        if (factory) {
            factory->List([&w](const char* type, const char* name, const char* config_json) {
                w.StartObject();
                w.Key("category"); w.String(type);
                w.Key("name"); w.String(name);
                w.Key("type"); w.String(ChannelType::kDatabase);
                // 从 config_json 提取 database 字段作为 schema 展示
                std::string db_label;
                if (config_json) {
                    rapidjson::Document cfg;
                    cfg.Parse(config_json);
                    if (!cfg.HasParseError() && cfg.IsObject()) {
                        if (cfg.HasMember("database") && cfg["database"].IsString()) {
                            db_label = cfg["database"].GetString();
                        } else if (cfg.HasMember("path") && cfg["path"].IsString()) {
                            db_label = cfg["path"].GetString();
                        }
                    }
                }
                w.Key("schema"); w.String(db_label.c_str());
                w.EndObject();
            });
        }

        // 流式通道（通过 IStreamFactory）
        auto* stream_factory = static_cast<IStreamFactory*>(querier_->First(IID_STREAM_FACTORY));
        if (stream_factory) {
            stream_factory->List([&w](const char* type, const char* name, IStreamChannel* stream_ch) {
                if (!stream_ch || !type || !name) return;
                w.StartObject();
                w.Key("category"); w.String(type);
                w.Key("name"); w.String(name);
                w.Key("type"); w.String(stream_ch->Type());
                w.Key("schema"); w.String(stream_ch->Schema());
                w.EndObject();
            });
        }
    }

    w.EndArray();
    rsp = buf.GetString();
    return error::OK;
}

// --- HandleQueryStreamChannelDefinitions ---
int32_t SchedulerPlugin::HandleQueryStreamChannelDefinitions(const std::string&,
                                                             const std::string&,
                                                             std::string& rsp) {
    auto* builtin_registry = querier_ ? static_cast<IBuiltinRegistry*>(querier_->First(IID_BUILTIN_REGISTRY)) : nullptr;
    if (!builtin_registry) {
        rsp = MakeErrorJson("builtin registry unavailable");
        return error::UNAVAILABLE;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("definitions");
    w.StartArray();

    builtin_registry->ListStreamChannelTypes([&w](const StreamChannelTypeDescriptor& def) {
        w.StartObject();
        w.Key("channel_type");
        w.String(def.type.c_str());
        w.Key("display_name");
        w.String(def.display_name.c_str());

        w.Key("allowed_roles");
        w.StartArray();
        for (const auto& role : def.allowed_roles) {
            w.String(role.c_str());
        }
        w.EndArray();

        w.Key("option_schema");
        w.StartArray();
        for (const auto& field : def.option_schema) {
            w.StartObject();
            w.Key("key");
            w.String(field.key.c_str());
            w.Key("type");
            w.String(field.type.c_str());
            w.Key("required");
            w.Bool(field.required);
            w.Key("default_value");
            w.String(field.default_value.c_str());
            w.Key("enum_values");
            w.StartArray();
            for (const auto& value : field.enum_values) {
                w.String(value.c_str());
            }
            w.EndArray();
            w.Key("min_value");
            w.Int64(field.min_value);
            w.Key("max_value");
            w.Int64(field.max_value);
            w.Key("has_range");
            w.Bool(field.has_range);
            w.Key("power_of_two");
            w.Bool(field.power_of_two);
            w.Key("desc");
            w.String(field.desc.c_str());
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
    });

    w.EndArray();
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

// --- HandleQueryStreamChannels ---
int32_t SchedulerPlugin::HandleQueryStreamChannels(const std::string&,
                                                   const std::string&,
                                                   std::string& rsp) {
    SweepFinishedTaskLeases();
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("channels");
    w.StartArray();

    auto* stream_manager = querier_ ? static_cast<IStreamManager*>(querier_->First(IID_STREAM_MANAGER)) : nullptr;
    auto* stream_factory = querier_ ? static_cast<IStreamFactory*>(querier_->First(IID_STREAM_FACTORY)) : nullptr;
    if (stream_manager) {
        stream_manager->QueryChannels([this, stream_factory, &w](const std::string& type,
                                                                 const std::string& name,
                                                                 const std::string& option,
                                                                 const std::string& status) {
            const std::string key = MakeStreamChannelKey(type, name);
            uint32_t in_use_count = 0;
            {
                std::lock_guard<std::mutex> lock(stream_channel_refs_mu_);
                auto it = stream_channel_ref_counts_.find(key);
                if (it != stream_channel_ref_counts_.end()) in_use_count = it->second;
            }
            IStreamChannel* stream_ch = stream_factory ? stream_factory->Get(type.c_str(), name.c_str()) : nullptr;
            const std::string role = ReadRoleFromOption(option);

            rapidjson::Document option_doc;
            std::string option_parse_err;
            const bool option_ok = (ParseOptionObject(option, &option_doc, &option_parse_err) == 0 && option_doc.IsObject());

            w.StartObject();
            w.Key("type");
            w.String(type.c_str());
            w.Key("name");
            w.String(name.c_str());
            w.Key("role");
            w.String(role.c_str());
            w.Key("option");
            w.String(option.c_str());
            w.Key("option_json");
            if (option_ok) {
                rapidjson::Document option_only;
                option_only.SetObject();
                auto& alloc = option_only.GetAllocator();
                for (auto it = option_doc.MemberBegin(); it != option_doc.MemberEnd(); ++it) {
                    if (std::string(it->name.GetString()) == "role") continue;
                    rapidjson::Value key_json;
                    key_json.SetString(it->name.GetString(), alloc);
                    rapidjson::Value val_json;
                    val_json.CopyFrom(it->value, alloc);
                    option_only.AddMember(key_json, val_json, alloc);
                }
                const std::string option_json = OptionObjectToJson(option_only);
                w.RawValue(option_json.c_str(), option_json.size(), rapidjson::kObjectType);
            } else {
                w.StartObject();
                w.EndObject();
            }
            w.Key("status");
            w.String(status.c_str());
            w.Key("in_use");
            w.Bool(in_use_count > 0);
            w.Key("capacity");
            w.Uint64(stream_ch ? stream_ch->Capacity() : 0);
            w.Key("size");
            w.Uint64(stream_ch ? stream_ch->Size() : 0);
            w.Key("is_finite");
            w.Bool(stream_ch ? stream_ch->IsFinite() : false);
            w.Key("is_finished");
            w.Bool(stream_ch ? stream_ch->IsFinished() : true);
            w.Key("derived_channels");
            w.StartArray();
            if (stream_ch && stream_ch->IsHubChannel() &&
                IEquals(stream_ch->HubModeHint() ? stream_ch->HubModeHint() : "", "split")) {
                for (size_t i = 0; i < stream_ch->HubPartitionCount(); ++i) {
                    auto partition = stream_ch->HubPartition(i);
                    if (!partition) continue;
                    std::string part_status = "running";
                    if (partition->IsFinished() && partition->IsEmpty()) {
                        part_status = "stopped";
                    } else if (partition->IsFinished()) {
                        part_status = "draining";
                    }
                    w.StartObject();
                    w.Key("index");
                    w.Uint(static_cast<unsigned>(i));
                    w.Key("name");
                    w.String(partition->Name());
                    w.Key("status");
                    w.String(part_status.c_str());
                    w.Key("capacity");
                    w.Uint64(partition->Capacity());
                    w.Key("size");
                    w.Uint64(partition->Size());
                    w.Key("is_finite");
                    w.Bool(partition->IsFinite());
                    w.Key("is_finished");
                    w.Bool(partition->IsFinished());
                    w.EndObject();
                }
            }
            w.EndArray();
            w.EndObject();
        });
    }

    w.EndArray();
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t SchedulerPlugin::HandleAddStreamChannel(const std::string&,
                                                const std::string& req,
                                                std::string& rsp) {
    auto* stream_manager = querier_ ? static_cast<IStreamManager*>(querier_->First(IID_STREAM_MANAGER)) : nullptr;
    if (!stream_manager) {
        rsp = MakeErrorJson("stream manager unavailable");
        return error::UNAVAILABLE;
    }

    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        rsp = MakeErrorJson("invalid request body");
        return error::BAD_REQUEST;
    }

    std::string type;
    std::string name;
    std::string role_raw;
    std::string option_legacy;
    const rapidjson::Value* options_obj = nullptr;
    const rapidjson::Value* cfg = (doc.HasMember("config") && doc["config"].IsObject()) ? &doc["config"] : nullptr;
    auto read_string = [&](const char* key, std::string* out) {
        if (!out) return;
        out->clear();
        if (doc.HasMember(key) && doc[key].IsString()) {
            *out = doc[key].GetString();
            return;
        }
        if (cfg && cfg->HasMember(key) && (*cfg)[key].IsString()) {
            *out = (*cfg)[key].GetString();
        }
    };
    read_string("type", &type);
    read_string("name", &name);
    read_string("role", &role_raw);
    read_string("option", &option_legacy);
    if (doc.HasMember("options") && doc["options"].IsObject()) {
        options_obj = &doc["options"];
    } else if (cfg && cfg->HasMember("options") && (*cfg)["options"].IsObject()) {
        options_obj = &(*cfg)["options"];
    }
    if (type.empty() || name.empty()) {
        rsp = MakeErrorJson("invalid request, expected {\"type\":\"...\",\"name\":\"...\",\"role\":\"...\",\"options\":{...}}");
        return error::BAD_REQUEST;
    }
    std::string role = role_raw.empty() ? "both" : NormalizeStreamRole(role_raw);
    if (role.empty()) {
        rsp = MakeErrorJson("invalid role, expected source|sink|both");
        return error::BAD_REQUEST;
    }
    if (!option_legacy.empty() && role_raw.empty()) {
        role = ReadRoleFromOption(option_legacy);
    }

    auto* builtin_registry = querier_ ? static_cast<IBuiltinRegistry*>(querier_->First(IID_BUILTIN_REGISTRY)) : nullptr;
    if (builtin_registry) {
        StreamChannelTypeDescriptor def;
        if (builtin_registry->FindStreamChannelType(type, &def) != 0) {
            rsp = MakeErrorJson("unsupported stream channel type: " + type);
            return error::BAD_REQUEST;
        }
        bool allowed = def.allowed_roles.empty();
        for (const auto& item : def.allowed_roles) {
            if (NormalizeStreamRole(item) == role) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            rsp = MakeErrorJson("role is not allowed for stream type: " + type);
            return error::BAD_REQUEST;
        }
    }

    std::string option;
    if (options_obj) {
        option = BuildOptionWithRoleJson(options_obj, role);
    } else if (!option_legacy.empty()) {
        rapidjson::Document option_doc;
        std::string parse_err;
        if (ParseOptionObject(option_legacy, &option_doc, &parse_err) != 0 || !option_doc.IsObject()) {
            rsp = MakeErrorJson("invalid option: " + parse_err);
            return error::BAD_REQUEST;
        }
        option = BuildOptionWithRoleJson(&option_doc, role);
    } else {
        option = BuildOptionWithRoleJson(nullptr, role);
    }

    const int rc = stream_manager->AddChannel(ToLowerAscii(type), name, option);
    if (rc != 0) {
        rsp = MakeErrorJson("add stream channel failed: " + type + "." + name);
        return MapStreamManagerErrorToStatus(rc);
    }
    {
        std::lock_guard<std::mutex> lock(stream_channel_refs_mu_);
        stream_channel_versions_[MakeStreamChannelKey(type, name)] += 1;
    }
    rsp = R"({"ok":true})";
    return error::OK;
}

int32_t SchedulerPlugin::HandleModifyStreamChannel(const std::string&,
                                                   const std::string& req,
                                                   std::string& rsp) {
    auto* stream_manager = querier_ ? static_cast<IStreamManager*>(querier_->First(IID_STREAM_MANAGER)) : nullptr;
    if (!stream_manager) {
        rsp = MakeErrorJson("stream manager unavailable");
        return error::UNAVAILABLE;
    }

    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        rsp = MakeErrorJson("invalid request body");
        return error::BAD_REQUEST;
    }

    std::string type;
    std::string name;
    std::string role_raw;
    std::string option_legacy;
    const rapidjson::Value* options_obj = nullptr;
    const rapidjson::Value* cfg = (doc.HasMember("config") && doc["config"].IsObject()) ? &doc["config"] : nullptr;
    auto read_string = [&](const char* key, std::string* out) {
        if (!out) return;
        out->clear();
        if (doc.HasMember(key) && doc[key].IsString()) {
            *out = doc[key].GetString();
            return;
        }
        if (cfg && cfg->HasMember(key) && (*cfg)[key].IsString()) {
            *out = (*cfg)[key].GetString();
        }
    };
    read_string("type", &type);
    read_string("name", &name);
    read_string("role", &role_raw);
    read_string("option", &option_legacy);
    if (doc.HasMember("options") && doc["options"].IsObject()) {
        options_obj = &doc["options"];
    } else if (cfg && cfg->HasMember("options") && (*cfg)["options"].IsObject()) {
        options_obj = &(*cfg)["options"];
    }
    if (type.empty() || name.empty()) {
        rsp = MakeErrorJson("invalid request, expected {\"type\":\"...\",\"name\":\"...\",\"role\":\"...\",\"options\":{...}}");
        return error::BAD_REQUEST;
    }
    std::string role = role_raw.empty() ? "both" : NormalizeStreamRole(role_raw);
    if (role.empty()) {
        rsp = MakeErrorJson("invalid role, expected source|sink|both");
        return error::BAD_REQUEST;
    }
    if (!option_legacy.empty() && role_raw.empty()) {
        role = ReadRoleFromOption(option_legacy);
    }

    auto* builtin_registry = querier_ ? static_cast<IBuiltinRegistry*>(querier_->First(IID_BUILTIN_REGISTRY)) : nullptr;
    if (builtin_registry) {
        StreamChannelTypeDescriptor def;
        if (builtin_registry->FindStreamChannelType(type, &def) != 0) {
            rsp = MakeErrorJson("unsupported stream channel type: " + type);
            return error::BAD_REQUEST;
        }
        bool allowed = def.allowed_roles.empty();
        for (const auto& item : def.allowed_roles) {
            if (NormalizeStreamRole(item) == role) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            rsp = MakeErrorJson("role is not allowed for stream type: " + type);
            return error::BAD_REQUEST;
        }
    }
    std::string option;
    if (options_obj) {
        option = BuildOptionWithRoleJson(options_obj, role);
    } else if (!option_legacy.empty()) {
        rapidjson::Document option_doc;
        std::string parse_err;
        if (ParseOptionObject(option_legacy, &option_doc, &parse_err) != 0 || !option_doc.IsObject()) {
            rsp = MakeErrorJson("invalid option: " + parse_err);
            return error::BAD_REQUEST;
        }
        option = BuildOptionWithRoleJson(&option_doc, role);
    } else {
        option = BuildOptionWithRoleJson(nullptr, role);
    }

    SweepFinishedTaskLeases();
    const std::string key = MakeStreamChannelKey(type, name);
    std::string mutation_reason;
    const int mutation_rc = TryBeginStreamChannelMutation(key, &mutation_reason);
    if (mutation_rc != 0) {
        if (mutation_reason == "source_in_use") {
            rsp = MakeExecutionErrorJson("stream source is in use", "STREAM_SOURCE_IN_USE", "modify");
            return error::CONFLICT;
        }
        if (mutation_reason == "mutating") {
            rsp = MakeExecutionErrorJson("stream channel is mutating", "STREAM_CHANNEL_MUTATING", "modify");
            return error::CONFLICT;
        }
        rsp = MakeExecutionErrorJson("stream channel is in use", "STREAM_CHANNEL_IN_USE", "modify");
        return error::CONFLICT;
    }
    auto mutation_guard = std::unique_ptr<void, std::function<void(void*)>>(
        reinterpret_cast<void*>(1),
        [this, key](void*) { EndStreamChannelMutation(key); });

    const int rc = stream_manager->ModifyChannel(ToLowerAscii(type), name, option);
    if (rc != 0) {
        rsp = MakeErrorJson("modify stream channel failed: " + type + "." + name);
        return MapStreamManagerErrorToStatus(rc);
    }
    mutation_guard.reset();
    rsp = R"({"ok":true})";
    return error::OK;
}

int32_t SchedulerPlugin::HandleRemoveStreamChannel(const std::string&,
                                                   const std::string& req,
                                                   std::string& rsp) {
    auto* stream_manager = querier_ ? static_cast<IStreamManager*>(querier_->First(IID_STREAM_MANAGER)) : nullptr;
    if (!stream_manager) {
        rsp = MakeErrorJson("stream manager unavailable");
        return error::UNAVAILABLE;
    }

    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() ||
        !doc.HasMember("type") || !doc["type"].IsString() ||
        !doc.HasMember("name") || !doc["name"].IsString()) {
        rsp = MakeErrorJson("invalid request, expected {\"type\":\"...\",\"name\":\"...\"}");
        return error::BAD_REQUEST;
    }
    const std::string type = doc["type"].GetString();
    const std::string name = doc["name"].GetString();

    SweepFinishedTaskLeases();
    const std::string key = MakeStreamChannelKey(type, name);
    std::string mutation_reason;
    const int mutation_rc = TryBeginStreamChannelMutation(key, &mutation_reason);
    if (mutation_rc != 0) {
        if (mutation_reason == "source_in_use") {
            rsp = MakeExecutionErrorJson("stream source is in use", "STREAM_SOURCE_IN_USE", "remove");
            return error::CONFLICT;
        }
        if (mutation_reason == "mutating") {
            rsp = MakeExecutionErrorJson("stream channel is mutating", "STREAM_CHANNEL_MUTATING", "remove");
            return error::CONFLICT;
        }
        rsp = MakeExecutionErrorJson("stream channel is in use", "STREAM_CHANNEL_IN_USE", "remove");
        return error::CONFLICT;
    }
    auto mutation_guard = std::unique_ptr<void, std::function<void(void*)>>(
        reinterpret_cast<void*>(1),
        [this, key](void*) { EndStreamChannelMutation(key); });

    const int rc = stream_manager->RemoveChannel(ToLowerAscii(type), name);
    if (rc != 0) {
        rsp = MakeErrorJson("remove stream channel failed: " + type + "." + name);
        return MapStreamManagerErrorToStatus(rc);
    }
    mutation_guard.reset();
    rsp = R"({"ok":true})";
    return error::OK;
}

// --- HandlePreviewDataframe ---
// POST /channels/dataframe/preview — Body: {"category":"...","name":"..."} 或 {"name":"..."}
int32_t SchedulerPlugin::HandlePreviewDataframe(const std::string&, const std::string& req, std::string& rsp) {
    auto* ch_registry = querier_ ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY)) : nullptr;
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("name") || !doc["name"].IsString()) {
        rsp = R"({"error":"missing 'name'"})";
        return error::BAD_REQUEST;
    }
    std::string category = "dataframe";
    if (doc.HasMember("category") && doc["category"].IsString()) {
        category = doc["category"].GetString();
    }
    std::string name = doc["name"].GetString();
    int page = 1;
    int page_size = 20;
    if (doc.HasMember("page") && doc["page"].IsInt()) page = doc["page"].GetInt();
    if (doc.HasMember("page_size") && doc["page_size"].IsInt()) page_size = doc["page_size"].GetInt();
    if (page < 1) page = 1;
    if (page_size < 1) page_size = 20;
    if (page_size > 100) page_size = 100;
    std::string key     = category + "." + name;

    // 先在内部通道表查找
    std::shared_ptr<IChannel> managed_holder = FindManagedChannelShared(key);
    IChannel* raw_ch = managed_holder.get();

    // 再去 IQuerier 静态注册通道查找
    if (!raw_ch && querier_) {
        querier_->Traverse(IID_CHANNEL, [&](void* p) -> int {
            auto* ch = static_cast<IChannel*>(p);
            if (IEquals(ch->Category(), category) && std::string(ch->Name()) == name) {
                raw_ch = ch;
                return 1;  // 找到，停止遍历
            }
            return 0;
        });
    }

    if (!raw_ch) {
        if (IEquals(category, "dataframe") && ch_registry) {
            auto named = ch_registry->Get(name.c_str());
            auto* named_df = dynamic_cast<IDataFrameChannel*>(named.get());
            if (named_df) raw_ch = named_df;
        }
    }

    if (!raw_ch) {
        rsp = "{\"error\":\"channel not found: " + key + "\"}";
        return error::NOT_FOUND;
    }

    auto* df_ch = dynamic_cast<IDataFrameChannel*>(raw_ch);
    if (!df_ch) {
        rsp = R"({"error":"not a dataframe channel"})";
        return error::BAD_REQUEST;
    }

    DataFrame data;
    if (df_ch->Read(&data) != 0 || data.RowCount() == 0) {
        rsp = R"({"columns":[],"types":[],"data":[],"rows":0})";
        return error::OK;
    }
    const auto schema = data.GetSchema();
    const int rows = data.RowCount();
    const int start = (page - 1) * page_size;
    const int end = std::min(rows, start + page_size);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("columns");
    w.StartArray();
    for (const auto& f : schema) w.String(f.name.c_str());
    w.EndArray();
    w.Key("types");
    w.StartArray();
    for (const auto& f : schema) w.String(DataTypeName(f.type));
    w.EndArray();
    w.Key("data");
    w.StartArray();
    for (int r = start; r < end; ++r) {
        const auto row = data.GetRow(r);
        w.StartArray();
        for (const auto& v : row) {
            std::visit(
                [&](auto&& val) {
                    using T = std::decay_t<decltype(val)>;
                    if constexpr (std::is_same_v<T, int32_t>) w.Int(val);
                    else if constexpr (std::is_same_v<T, int64_t>) w.Int64(val);
                    else if constexpr (std::is_same_v<T, uint32_t>) w.Uint(val);
                    else if constexpr (std::is_same_v<T, uint64_t>) w.Uint64(val);
                    else if constexpr (std::is_same_v<T, float>) w.Double(val);
                    else if constexpr (std::is_same_v<T, double>) w.Double(val);
                    else if constexpr (std::is_same_v<T, std::string>) w.String(val.c_str());
                    else if constexpr (std::is_same_v<T, std::vector<uint8_t>>)
                        w.String(reinterpret_cast<const char*>(val.data()), val.size());
                    else if constexpr (std::is_same_v<T, bool>) w.Bool(val);
                },
                v);
        }
        w.EndArray();
    }
    w.EndArray();
    w.Key("rows");
    w.Int(rows);
    w.Key("page");
    w.Int(page);
    w.Key("page_size");
    w.Int(page_size);
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}
// --- HandleRefreshOperators ---
int32_t SchedulerPlugin::HandleRefreshOperators(const std::string&, const std::string&, std::string& rsp) {
    if (!querier_) {
        rsp = R"({"error":"querier not initialized"})";
        return error::INTERNAL_ERROR;
    }
    auto* bridge = static_cast<IBridge*>(querier_->First(IID_BRIDGE));
    if (bridge) {
        int rc = bridge->Refresh();
        if (rc == 0) {
            rsp = R"({"status":"refreshed"})";
            return error::OK;
        } else {
            rsp = R"({"error":"refresh failed"})";
            return error::INTERNAL_ERROR;
        }
    } else {
        rsp = R"({"error":"bridge not available"})";
        return error::NOT_FOUND;
    }
}

// ==================== 数据库通道动态管理端点（Epic 6）====================
// 已移交 DatabasePlugin 处理，此处删除

}  // namespace scheduler
}  // namespace flowsql
