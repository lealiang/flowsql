#include "scheduler_plugin.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <framework/core/fan_in_stream_channel.h>
#include <framework/core/ring_stream_channel.h>
#include <framework/core/sql_parser.h>
#include <framework/core/sql_text_splitter.h>

namespace flowsql {
namespace scheduler {

namespace {

constexpr size_t kDefaultMaxGroupNodes = 64;
constexpr size_t kDefaultMaxGroupEdges = 256;
constexpr size_t kDefaultMaxGroupShareSets = 16;
constexpr size_t kDefaultMaxGroupSqlBytes = 256 * 1024;
constexpr int kDefaultShareSetReadyTimeoutS = 30;

struct NodeResolvedMeta {
    std::vector<std::string> source_keys;
    std::vector<std::string> resolved_sources;
    std::string expand_rule = "explicit";
    std::vector<std::shared_ptr<IStreamChannel>> stream_channels;
};

struct ShareSetPlan {
    std::string id;
    std::string source_ref;
    std::vector<std::string> members;
    std::vector<std::string> canonical_source_keys;
    std::vector<std::shared_ptr<IStreamChannel>> source_channels;
};

std::string ToLowerAsciiLocal(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool StartsWithIgnoreCaseLocal(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

std::string MakeErrorJsonLocal(const std::string& error) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error.c_str());
    w.EndObject();
    return buf.GetString();
}

std::string MakeExecutionErrorJsonLocal(const std::string& error,
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

std::string MakeExecutionErrorWithSqlIndexJsonLocal(const std::string& error,
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

std::string MakeSinkCapabilityMismatchErrorJsonLocal(
    const std::string& error,
    const std::string& error_stage,
    const std::string& sink_key,
    uint32_t required_writers,
    ProducerMode actual_put_mode,
    uint32_t actual_max_producers) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error.c_str());
    w.Key("error_code");
    w.String("STREAM_GROUP_SINK_CAPABILITY_MISMATCH");
    w.Key("error_stage");
    w.String(error_stage.c_str());
    w.Key("sink_key");
    w.String(sink_key.c_str());
    w.Key("required");
    w.StartObject();
    w.Key("writers");
    w.Uint(required_writers);
    w.Key("put_mode");
    w.String("MULTI");
    w.EndObject();
    w.Key("actual");
    w.StartObject();
    w.Key("put_mode");
    w.String(actual_put_mode == ProducerMode::MULTI ? "MULTI" : "SINGLE");
    w.Key("max_producers");
    w.Uint(actual_max_producers);
    w.EndObject();
    w.EndObject();
    return buf.GetString();
}

std::string ExtractErrorMessage(const std::string& json) {
    rapidjson::Document d;
    d.Parse(json.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("error") || !d["error"].IsString()) {
        return "";
    }
    return d["error"].GetString();
}

bool ExtractRuntimeTaskId(const std::string& json, std::string* runtime_task_id) {
    if (!runtime_task_id) return false;
    runtime_task_id->clear();
    rapidjson::Document d;
    d.Parse(json.c_str());
    if (d.HasParseError() || !d.IsObject()) return false;
    if (d.HasMember("runtime_task_id") && d["runtime_task_id"].IsString()) {
        *runtime_task_id = d["runtime_task_id"].GetString();
        return !runtime_task_id->empty();
    }
    if (d.HasMember("task_id") && d["task_id"].IsString()) {
        *runtime_task_id = d["task_id"].GetString();
        return !runtime_task_id->empty();
    }
    return false;
}

bool ParseStartCondition(const rapidjson::Value& node_v,
                         GroupStartCondition* out,
                         std::string* err_msg) {
    if (!out) return false;
    *out = GroupStartCondition::kOnRunning;
    if (!node_v.IsObject() || !node_v.HasMember("start_condition")) return true;
    if (!node_v["start_condition"].IsString()) {
        if (err_msg) *err_msg = "start_condition must be string";
        return false;
    }
    const std::string cond = ToLowerAsciiLocal(node_v["start_condition"].GetString());
    if (cond == "on_running") {
        *out = GroupStartCondition::kOnRunning;
        return true;
    }
    if (cond == "on_finished") {
        *out = GroupStartCondition::kOnFinished;
        return true;
    }
    if (err_msg) *err_msg = "start_condition must be on_running or on_finished";
    return false;
}

std::vector<std::string> CanonicalSourceKeySet(const std::vector<std::string>& keys) {
    std::set<std::string> uniq;
    for (const auto& k : keys) {
        if (!k.empty()) uniq.insert(k);
    }
    return std::vector<std::string>(uniq.begin(), uniq.end());
}

void ComputeSetDiff(const std::vector<std::string>& expected,
                    const std::vector<std::string>& actual,
                    std::vector<std::string>* missing,
                    std::vector<std::string>* extra) {
    if (missing) {
        missing->clear();
        std::set_difference(expected.begin(), expected.end(),
                            actual.begin(), actual.end(),
                            std::back_inserter(*missing));
    }
    if (extra) {
        extra->clear();
        std::set_difference(actual.begin(), actual.end(),
                            expected.begin(), expected.end(),
                            std::back_inserter(*extra));
    }
}

std::string MakeSourceMismatchErrorJsonLocal(const std::string& error,
                                             const std::string& error_stage,
                                             const std::string& share_set_id,
                                             const std::string& node_id,
                                             const std::vector<std::string>& expected_keys,
                                             const std::vector<std::string>& actual_keys) {
    std::vector<std::string> missing_keys;
    std::vector<std::string> extra_keys;
    ComputeSetDiff(expected_keys, actual_keys, &missing_keys, &extra_keys);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error.c_str());
    w.Key("error_code");
    w.String("STREAM_GROUP_SOURCE_MISMATCH");
    w.Key("error_stage");
    w.String(error_stage.c_str());
    if (!share_set_id.empty()) {
        w.Key("share_set_id");
        w.String(share_set_id.c_str());
    }
    if (!node_id.empty()) {
        w.Key("node_id");
        w.String(node_id.c_str());
    }
    w.Key("missing_keys");
    w.StartArray();
    for (const auto& key : missing_keys) {
        w.String(key.c_str());
    }
    w.EndArray();
    w.Key("extra_keys");
    w.StartArray();
    for (const auto& key : extra_keys) {
        w.String(key.c_str());
    }
    w.EndArray();
    w.EndObject();
    return buf.GetString();
}

std::string JoinStrings(const std::vector<std::string>& values, const char* sep) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out += sep;
        out += values[i];
    }
    return out;
}

std::string MakeSafeName(const std::string& input) {
    std::string out = input;
    for (auto& c : out) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) {
            c = '_';
        }
    }
    return out;
}

int64_t CurrentTimeMsLocal() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool ParseChannelBaseLocal(const std::string& ref, std::string* base, std::string* err) {
    if (!base) return false;
    if (err) err->clear();
    base->clear();
    if (ref.empty()) {
        if (err) *err = "empty channel reference";
        return false;
    }
    const auto lb = ref.find('[');
    if (lb == std::string::npos) {
        *base = ref;
        return true;
    }
    const auto rb = ref.find(']', lb + 1);
    if (rb == std::string::npos || rb + 1 != ref.size()) {
        if (err) *err = "invalid channel selector";
        return false;
    }
    if (ref.find('[', lb + 1) != std::string::npos) {
        if (err) *err = "invalid channel selector";
        return false;
    }
    *base = ref.substr(0, lb);
    if (base->empty()) {
        if (err) *err = "invalid channel selector";
        return false;
    }
    return true;
}

std::string CanonicalKeysHash(const std::vector<std::string>& keys) {
    return JoinStrings(keys, "\x1f");
}

std::string MakeStreamChannelKeyLocal(const std::string& type, const std::string& name) {
    return ToLowerAsciiLocal(type) + "." + name;
}

}  // namespace
int32_t SchedulerPlugin::HandleStreamExecuteGroup(const rapidjson::Document& doc, std::string& rsp) {
    if (!doc.HasMember("group_mode") || !doc["group_mode"].IsString()) {
        rsp = MakeExecutionErrorJsonLocal(
            "group_mode must be provided for group execution",
            "STREAM_GROUP_MODE_INVALID",
            "request");
        return error::BAD_REQUEST;
    }
    const std::string group_mode = ToLowerAsciiLocal(doc["group_mode"].GetString());
    if (group_mode != "dag") {
        rsp = MakeExecutionErrorJsonLocal(
            "only group_mode=dag is supported",
            "STREAM_GROUP_MODE_INVALID",
            "request");
        return error::BAD_REQUEST;
    }

    if (doc.HasMember("dag") ||
        doc.HasMember("nodes") ||
        doc.HasMember("source_share_sets") ||
        doc.HasMember("sql") ||
        doc.HasMember("sqls")) {
        rsp = MakeExecutionErrorJsonLocal(
            "group execution accepts only sql_text/group_mode/timeout fields",
            "STREAM_GROUP_SQL_TEXT_INVALID",
            "request");
        return error::BAD_REQUEST;
    }
    if (!doc.HasMember("sql_text") || !doc["sql_text"].IsString()) {
        rsp = MakeExecutionErrorJsonLocal(
            "group execution requires sql_text",
            "STREAM_GROUP_SQL_TEXT_INVALID",
            "request");
        return error::BAD_REQUEST;
    }

    int timeout_s = 0;
    if (doc.HasMember("timeout_s")) {
        if (!doc["timeout_s"].IsInt()) {
            rsp = MakeExecutionErrorJsonLocal(
                "timeout_s must be integer",
                "STREAM_GROUP_SQL_TEXT_INVALID",
                "request");
            return error::BAD_REQUEST;
        }
        timeout_s = doc["timeout_s"].GetInt();
        if (timeout_s < 0) {
            rsp = MakeExecutionErrorJsonLocal(
                "timeout_s must be >= 0",
                "STREAM_GROUP_SQL_TEXT_INVALID",
                "request");
            return error::BAD_REQUEST;
        }
        if (timeout_s > max_stream_group_timeout_s_) {
            rsp = MakeExecutionErrorJsonLocal(
                "timeout_s exceeds max_stream_group_timeout_s: " +
                    std::to_string(max_stream_group_timeout_s_),
                "STREAM_GROUP_SQL_TEXT_INVALID",
                "request");
            return error::BAD_REQUEST;
        }
    }

    int share_set_ready_timeout_s = kDefaultShareSetReadyTimeoutS;
    if (doc.HasMember("share_set_ready_timeout_s")) {
        if (!doc["share_set_ready_timeout_s"].IsInt()) {
            rsp = MakeExecutionErrorJsonLocal(
                "share_set_ready_timeout_s must be integer",
                "STREAM_GROUP_SQL_TEXT_INVALID",
                "request");
            return error::BAD_REQUEST;
        }
        share_set_ready_timeout_s = doc["share_set_ready_timeout_s"].GetInt();
        if (share_set_ready_timeout_s <= 0) {
            rsp = MakeExecutionErrorJsonLocal(
                "share_set_ready_timeout_s must be > 0",
                "STREAM_GROUP_SQL_TEXT_INVALID",
                "request");
            return error::BAD_REQUEST;
        }
    }
    if (timeout_s > 0 && share_set_ready_timeout_s > timeout_s) {
        share_set_ready_timeout_s = timeout_s;
    }

    std::vector<std::string> sqls;
    SqlTextSplitError split_err;
    if (SplitSqlText(doc["sql_text"].GetString(), &sqls, &split_err) != 0) {
        std::string err = "invalid sql_text";
        if (!split_err.message.empty()) {
            err += ": " + split_err.message;
        }
        rsp = MakeExecutionErrorWithSqlIndexJsonLocal(
            err,
            "STREAM_GROUP_SQL_TEXT_INVALID",
            "request",
            split_err.statement_index);
        return error::BAD_REQUEST;
    }
    if (sqls.size() < 2) {
        rsp = MakeExecutionErrorJsonLocal(
            "group execution requires at least two SQL statements",
            "STREAM_GROUP_SQL_TEXT_INVALID",
            "request");
        return error::BAD_REQUEST;
    }
    if (sqls.size() > kDefaultMaxGroupNodes) {
        rsp = MakeExecutionErrorJsonLocal(
            "group nodes exceed max_group_nodes",
            "STREAM_GROUP_DAG_TOO_LARGE",
            "dag_validate");
        return error::BAD_REQUEST;
    }

    std::vector<GroupNodePlan> plans;
    plans.reserve(sqls.size());
    std::unordered_map<std::string, size_t> node_index;
    std::unordered_map<std::string, NodeResolvedMeta> node_resolved;
    std::unordered_map<std::string, uint32_t> sink_writer_counts;
    std::unordered_map<std::string, StreamChannelCapabilities> sink_caps_map;
    std::unordered_map<std::string, std::vector<size_t>> stream_sink_producers;
    std::vector<std::string> node_stream_sink_keys;
    node_stream_sink_keys.reserve(sqls.size());

    size_t edges = 0;
    size_t sql_bytes = 0;

    for (size_t i = 0; i < sqls.size(); ++i) {
        GroupNodePlan plan;
        plan.id = "n" + std::to_string(i + 1);
        plan.sql = sqls[i];
        node_index.emplace(plan.id, plans.size());

        sql_bytes += plan.sql.size();
        if (sql_bytes > kDefaultMaxGroupSqlBytes) {
            rsp = MakeExecutionErrorJsonLocal(
                "group sql bytes exceed max_group_sql_bytes",
                "STREAM_GROUP_DAG_TOO_LARGE",
                "dag_validate");
            return error::BAD_REQUEST;
        }

        SqlParser parser;
        SqlStatement parsed = parser.Parse(plan.sql);
        if (!parsed.error.empty()) {
            rsp = MakeExecutionErrorWithSqlIndexJsonLocal(
                "group node SQL parse failed: node=" + plan.id + ", " + parsed.error,
                "STREAM_GROUP_DAG_INVALID",
                "dag_validate",
                i);
            return error::BAD_REQUEST;
        }
        if (parsed.sources.empty() && !parsed.source.empty()) {
            parsed.sources.push_back(parsed.source);
        }
        if (parsed.sources.empty()) {
            rsp = MakeExecutionErrorWithSqlIndexJsonLocal(
                "group node source channel not found: node=" + plan.id,
                "STREAM_GROUP_DAG_INVALID",
                "dag_validate",
                i);
            return error::BAD_REQUEST;
        }

        std::unordered_set<std::string> dep_ids;
        for (const auto& source_ref : parsed.sources) {
            std::string source_base;
            std::string source_base_err;
            if (!ParseChannelBaseLocal(source_ref, &source_base, &source_base_err)) {
                rsp = MakeExecutionErrorJsonLocal(
                    "group node source selector invalid: node=" + plan.id + ", source=" + source_ref,
                    "STREAM_GROUP_DAG_INVALID",
                    "dag_validate");
                return error::BAD_REQUEST;
            }
            if (!StartsWithIgnoreCaseLocal(source_base, "stream.")) continue;
            auto it = stream_sink_producers.find(ToLowerAsciiLocal(source_base));
            if (it == stream_sink_producers.end()) continue;
            if (it->second.size() > 1) {
                rsp = MakeExecutionErrorJsonLocal(
                    "ambiguous upstream stream source: node=" + plan.id + ", source=" + source_base,
                    "STREAM_GROUP_DAG_INVALID",
                    "dag_validate");
                return error::BAD_REQUEST;
            }
            const size_t dep_idx = it->second.front();
            if (dep_idx >= plans.size()) {
                rsp = MakeExecutionErrorJsonLocal(
                    "dependency index out of range: node=" + plan.id,
                    "STREAM_GROUP_DAG_INVALID",
                    "dag_validate");
                return error::BAD_REQUEST;
            }
            dep_ids.insert(plans[dep_idx].id);
        }

        if (!dep_ids.empty()) {
            std::vector<size_t> dep_order;
            dep_order.reserve(dep_ids.size());
            for (const auto& dep_id : dep_ids) {
                auto dep_it = node_index.find(dep_id);
                if (dep_it != node_index.end()) dep_order.push_back(dep_it->second);
            }
            std::sort(dep_order.begin(), dep_order.end());
            dep_order.erase(std::unique(dep_order.begin(), dep_order.end()), dep_order.end());
            for (size_t dep_idx : dep_order) {
                plan.depends_on.push_back(plans[dep_idx].id);
            }
        }

        edges += plan.depends_on.size();
        if (edges > kDefaultMaxGroupEdges) {
            rsp = MakeExecutionErrorJsonLocal(
                "group edges exceed max_group_edges",
                "STREAM_GROUP_DAG_TOO_LARGE",
                "dag_validate");
            return error::BAD_REQUEST;
        }

        SourceResolveResult source_resolved;
        std::string source_err_rsp;
        const int32_t source_rc = ResolveSourceBindings(parsed, &source_resolved, &source_err_rsp);
        if (source_rc != error::OK) {
            const std::string node_err = ExtractErrorMessage(source_err_rsp);
            rsp = MakeExecutionErrorJsonLocal(
                "group node source resolve failed: node=" + plan.id +
                    (node_err.empty() ? "" : (", " + node_err)),
                "STREAM_GROUP_DAG_INVALID",
                "dag_validate");
            return source_rc;
        }
        if (!source_resolved.has_stream_source || source_resolved.has_non_stream_source) {
            rsp = MakeExecutionErrorJsonLocal(
                "group node must be stream task kind: node=" + plan.id,
                "STREAM_GROUP_MIXED_TASK_KIND",
                "dag_validate");
            return error::BAD_REQUEST;
        }

        NodeResolvedMeta meta;
        meta.source_keys = source_resolved.source_keys;
        meta.resolved_sources = source_resolved.resolved_sources;
        meta.expand_rule = source_resolved.source_expand_rule;
        meta.stream_channels = source_resolved.stream_channels;
        node_resolved[plan.id] = std::move(meta);

        std::string sink_base;
        std::string sink_base_err;
        if (!ParseChannelBaseLocal(parsed.dest, &sink_base, &sink_base_err)) {
            rsp = MakeExecutionErrorJsonLocal(
                "group node sink selector invalid: node=" + plan.id + ", sink=" + parsed.dest,
                "STREAM_GROUP_DAG_INVALID",
                "dag_validate");
            return error::BAD_REQUEST;
        }

        if (StartsWithIgnoreCaseLocal(sink_base, "stream.")) {
            std::shared_ptr<IChannel> sink_owner;
            IChannel* sink_ch = FindChannel(sink_base, &sink_owner);
            auto* sink_stream = dynamic_cast<IStreamChannel*>(sink_ch);
            if (!sink_stream) {
                rsp = MakeExecutionErrorJsonLocal(
                    "group node stream sink not found: node=" + plan.id + ", sink=" + sink_base,
                    "STREAM_GROUP_DAG_INVALID",
                    "dag_validate");
                return error::BAD_REQUEST;
            }
            const std::string sink_key = MakeStreamChannelKeyLocal(sink_stream->Category(), sink_stream->Name());
            sink_writer_counts[sink_key] += 1;
            sink_caps_map[sink_key] = sink_stream->Capabilities();
            stream_sink_producers[ToLowerAsciiLocal(sink_base)].push_back(i);
            node_stream_sink_keys.push_back(sink_key);
        } else {
            node_stream_sink_keys.push_back("");
        }

        plans.push_back(std::move(plan));
    }

    for (const auto& kv : sink_writer_counts) {
        if (kv.second <= 1) continue;
        const auto caps_it = sink_caps_map.find(kv.first);
        if (caps_it == sink_caps_map.end()) continue;
        const auto& caps = caps_it->second;
        const bool put_mode_ok = caps.concurrency.put_mode == ProducerMode::MULTI;
        const bool producers_ok = caps.concurrency.max_producers == 0 ||
                                  caps.concurrency.max_producers >= kv.second;
        if (!put_mode_ok || !producers_ok) {
            rsp = MakeSinkCapabilityMismatchErrorJsonLocal(
                "shared stream sink capability mismatch: sink=" + kv.first +
                    ", writers=" + std::to_string(kv.second) +
                    ", required.put_mode=MULTI, actual.put_mode=" +
                    std::string(caps.concurrency.put_mode == ProducerMode::MULTI ? "MULTI" : "SINGLE") +
                    ", actual.max_producers=" + std::to_string(caps.concurrency.max_producers),
                "capability_check",
                kv.first,
                kv.second,
                caps.concurrency.put_mode,
                caps.concurrency.max_producers);
            return error::BAD_REQUEST;
        }
    }

    std::vector<uint32_t> indegree(plans.size(), 0);
    std::vector<std::vector<size_t>> graph(plans.size());
    for (size_t i = 0; i < plans.size(); ++i) {
        std::unordered_set<std::string> dep_dedup;
        for (const auto& dep : plans[i].depends_on) {
            if (dep == plans[i].id) {
                rsp = MakeExecutionErrorJsonLocal(
                    "self dependency is not allowed: node=" + plans[i].id,
                    "STREAM_GROUP_DAG_INVALID",
                    "dag_validate");
                return error::BAD_REQUEST;
            }
            if (!dep_dedup.insert(dep).second) continue;
            auto dep_it = node_index.find(dep);
            if (dep_it == node_index.end()) {
                rsp = MakeExecutionErrorJsonLocal(
                    "dependency node not found: " + dep + ", node=" + plans[i].id,
                    "STREAM_GROUP_NODE_NOT_FOUND",
                    "dag_validate");
                return error::BAD_REQUEST;
            }
            graph[dep_it->second].push_back(i);
            indegree[i] += 1;
        }
    }

    std::queue<size_t> q;
    for (size_t i = 0; i < indegree.size(); ++i) {
        if (indegree[i] == 0) q.push(i);
    }
    size_t visited = 0;
    while (!q.empty()) {
        const size_t u = q.front();
        q.pop();
        ++visited;
        for (size_t v : graph[u]) {
            if (--indegree[v] == 0) q.push(v);
        }
    }
    if (visited != plans.size()) {
        rsp = MakeExecutionErrorJsonLocal(
            "dag has cycle dependency",
            "STREAM_GROUP_DAG_CYCLE_DETECTED",
            "dag_validate");
        return error::BAD_REQUEST;
    }

    std::vector<ShareSetPlan> share_set_plans;
    std::map<std::string, std::vector<size_t>> root_groups;
    for (size_t i = 0; i < plans.size(); ++i) {
        if (!plans[i].depends_on.empty()) continue;
        const auto resolved_it = node_resolved.find(plans[i].id);
        if (resolved_it == node_resolved.end()) continue;
        const auto canonical = CanonicalSourceKeySet(resolved_it->second.source_keys);
        if (canonical.empty()) {
            rsp = MakeSourceMismatchErrorJsonLocal(
                "root node has empty canonical source keys: node=" + plans[i].id,
                "dag_validate",
                "",
                plans[i].id,
                {},
                {});
            return error::BAD_REQUEST;
        }
        root_groups[CanonicalKeysHash(canonical)].push_back(i);
    }

    int share_set_seq = 0;
    for (const auto& group_item : root_groups) {
        const auto& members = group_item.second;
        if (members.size() < 2) continue;

        ShareSetPlan ss;
        ss.id = "s" + std::to_string(++share_set_seq);

        const size_t first_idx = members.front();
        const auto resolved_it = node_resolved.find(plans[first_idx].id);
        if (resolved_it == node_resolved.end()) continue;

        ss.canonical_source_keys = CanonicalSourceKeySet(resolved_it->second.source_keys);
        ss.source_ref = JoinStrings(resolved_it->second.resolved_sources, ",");
        ss.source_channels = resolved_it->second.stream_channels;
        if (ss.source_channels.empty()) {
            rsp = MakeSourceMismatchErrorJsonLocal(
                "auto source_share_set has empty source channels: set=" + ss.id,
                "dag_validate",
                ss.id,
                plans[first_idx].id,
                ss.canonical_source_keys,
                {});
            return error::BAD_REQUEST;
        }

        for (size_t member_idx : members) {
            const std::string& member_node_id = plans[member_idx].id;
            const auto member_it = node_resolved.find(member_node_id);
            if (member_it == node_resolved.end()) continue;
            const auto member_keys = CanonicalSourceKeySet(member_it->second.source_keys);
            if (member_keys != ss.canonical_source_keys) {
                rsp = MakeSourceMismatchErrorJsonLocal(
                    "source_share_set canonical source mismatch: set=" + ss.id + ", node=" + member_node_id,
                    "dag_validate",
                    ss.id,
                    member_node_id,
                    ss.canonical_source_keys,
                    member_keys);
                return error::BAD_REQUEST;
            }
            ss.members.push_back(member_node_id);
        }
        share_set_plans.push_back(std::move(ss));
    }
    if (share_set_plans.size() > kDefaultMaxGroupShareSets) {
        rsp = MakeExecutionErrorJsonLocal(
            "auto source_share_sets exceed max_group_share_sets",
            "STREAM_GROUP_DAG_TOO_LARGE",
            "dag_validate");
        return error::BAD_REQUEST;
    }

    const std::string runtime_task_id = NextStreamTaskId();
    std::vector<std::string> group_source_keys;
    std::vector<std::string> group_sink_keys;
    for (size_t i = 0; i < plans.size(); ++i) {
        auto resolved_it = node_resolved.find(plans[i].id);
        if (resolved_it != node_resolved.end()) {
            group_source_keys.insert(group_source_keys.end(),
                                     resolved_it->second.source_keys.begin(),
                                     resolved_it->second.source_keys.end());
        }
        if (i < node_stream_sink_keys.size() && !node_stream_sink_keys[i].empty()) {
            group_sink_keys.push_back(node_stream_sink_keys[i]);
        }
    }

    SweepFinishedTaskLeases();
    std::vector<std::string> lease_keys;
    lease_keys.reserve(group_source_keys.size() + group_sink_keys.size());
    lease_keys.insert(lease_keys.end(), group_source_keys.begin(), group_source_keys.end());
    lease_keys.insert(lease_keys.end(), group_sink_keys.begin(), group_sink_keys.end());
    std::unordered_map<std::string, uint64_t> version_snapshot;
    CaptureStreamChannelVersionSnapshot(lease_keys, &version_snapshot);

    std::string conflict_key;
    std::string version_conflict_key;
    bool blocked_by_mutation = false;
    const int lease_rc = TryAcquireStreamTaskLeases(runtime_task_id,
                                                    group_source_keys,
                                                    group_sink_keys,
                                                    &conflict_key,
                                                    &blocked_by_mutation,
                                                    runtime_task_id,
                                                    &version_snapshot,
                                                    &version_conflict_key);
    if (lease_rc != 0) {
        if (lease_rc == EBUSY) {
            if (blocked_by_mutation) {
                rsp = MakeExecutionErrorJsonLocal(
                    "stream channel is being modified: " + conflict_key,
                    "STREAM_CHANNEL_MUTATING",
                    "lease");
                return error::CONFLICT;
            }
            rsp = MakeExecutionErrorJsonLocal(
                "stream source is in use: " + conflict_key,
                "STREAM_SOURCE_IN_USE",
                "lease");
            return error::CONFLICT;
        }
        if (lease_rc == EAGAIN) {
            rsp = MakeExecutionErrorJsonLocal(
                "stream channel changed during group prepare: " + version_conflict_key,
                "STREAM_CHANNEL_VERSION_CHANGED",
                "lease");
            return error::CONFLICT;
        }
        rsp = MakeExecutionErrorJsonLocal(
            "stream group lease acquire failed",
            "STREAM_LEASE_FAILED",
            "lease");
        return error::INTERNAL_ERROR;
    }

    bool release_group_lease_on_fail = true;
    auto group_lease_guard = std::unique_ptr<void, std::function<void(void*)>>(
        reinterpret_cast<void*>(1),
        [this, runtime_task_id, &release_group_lease_on_fail](void*) {
            if (release_group_lease_on_fail) {
                ReleaseStreamTaskLeases(runtime_task_id);
            }
        });

    std::unordered_map<std::string, std::string> node_source_overrides;
    std::vector<StreamGroupShareSetRuntime> share_set_runtimes;
    std::vector<std::string> created_channel_refs;
    std::unordered_set<std::string> channel_ref_dedup;
    auto cleanup_local_resources = [&]() {
        for (auto& ss : share_set_runtimes) {
            if (ss.hub) {
                ss.hub->RequestStop();
                ss.hub->Join();
            }
        }
        for (const auto& channel_ref : created_channel_refs) {
            EraseManagedChannel(channel_ref);
        }
        created_channel_refs.clear();
    };

    for (const auto& ss : share_set_plans) {
        std::shared_ptr<IStreamChannel> shared_source = ss.source_channels.front();
        std::shared_ptr<FanInStreamChannel> fanin;
        if (ss.source_channels.size() > 1) {
            fanin = std::make_shared<FanInStreamChannel>(
                "fanin",
                runtime_task_id + "." + ss.id + ".fanin",
                ss.source_channels);
            shared_source = fanin;
        }

        StreamGroupShareSetRuntime runtime;
        runtime.id = ss.id;
        runtime.source_ref = ss.source_ref;
        runtime.members = ss.members;

        std::vector<std::shared_ptr<IStreamChannel>> member_channels;
        member_channels.reserve(ss.members.size());
        for (size_t mi = 0; mi < ss.members.size(); ++mi) {
            const std::string internal_name = MakeSafeName(
                runtime_task_id + "_" + ss.id + "_" + ss.members[mi] + "_in");
            const std::string channel_ref = "stream." + internal_name;
            if (!channel_ref_dedup.insert(channel_ref).second) {
                rsp = MakeExecutionErrorJsonLocal(
                    "duplicate internal stream channel reference: " + channel_ref,
                    "STREAM_GROUP_BRANCH_BUILD_FAILED",
                    "branch_build");
                cleanup_local_resources();
                return error::INTERNAL_ERROR;
            }
            RingStreamChannelOptions opts;
            opts.ring_size = 2048;
            opts.batch_rows = 1024;
            opts.overflow = OverflowPolicy::kDrop;
            opts.ring_mode = RingMode::SPSC;
            opts.finite = false;
            auto internal = std::make_shared<RingStreamChannel>("ring", internal_name, opts);
            const int open_rc = internal->Open();
            if (open_rc != 0) {
                rsp = MakeExecutionErrorJsonLocal(
                    "open internal stream channel failed: " + channel_ref,
                    "STREAM_GROUP_BRANCH_BUILD_FAILED",
                    "branch_build");
                cleanup_local_resources();
                return error::INTERNAL_ERROR;
            }
            RegisterChannel(channel_ref, std::static_pointer_cast<IChannel>(internal));
            created_channel_refs.push_back(channel_ref);
            runtime.internal_channel_refs.push_back(channel_ref);
            member_channels.push_back(internal);
            node_source_overrides[ss.members[mi]] = channel_ref;
        }

        runtime.hub = std::make_shared<BroadcastHub>(
            ss.id,
            ss.source_ref,
            ss.members,
            shared_source,
            member_channels);
        share_set_runtimes.push_back(std::move(runtime));
    }

    auto stop_group_hubs = [this, runtime_task_id]() {
        std::vector<std::shared_ptr<BroadcastHub>> hubs;
        {
            std::lock_guard<std::mutex> lock(stream_group_share_sets_mu_);
            auto it = stream_group_share_sets_.find(runtime_task_id);
            if (it != stream_group_share_sets_.end()) {
                for (const auto& ss : it->second) {
                    if (ss.hub) hubs.push_back(ss.hub);
                }
            }
        }
        for (auto& hub : hubs) {
            hub->RequestStop();
            hub->Join();
        }
    };

    auto group = std::make_shared<StreamTaskGroup>(
        runtime_task_id,
        runtime_task_id,
        plans,
        timeout_s,
        [this, runtime_task_id, node_source_overrides](const std::string& node_id,
                                                       const std::string& sql,
                                                       std::string* node_runtime_task_id,
                                                       std::string* error_msg) -> int {
            SqlParser parser;
            SqlStatement stmt = parser.Parse(sql);
            if (!stmt.error.empty()) {
                if (error_msg) *error_msg = "node " + node_id + " SQL parse failed: " + stmt.error;
                return EINVAL;
            }
            if (stmt.sources.empty() && !stmt.source.empty()) {
                stmt.sources.push_back(stmt.source);
            }
            if (stmt.sources.empty()) {
                if (error_msg) *error_msg = "node " + node_id + " source channel not found";
                return EINVAL;
            }
            auto override_it = node_source_overrides.find(node_id);
            if (override_it != node_source_overrides.end()) {
                stmt.source = override_it->second;
                stmt.sources.clear();
                stmt.sources.push_back(override_it->second);
            }

            std::string exec_rsp;
            const int32_t rc = ExecuteStreamTask(stmt, exec_rsp, runtime_task_id, true);
            if (rc != error::OK) {
                const std::string err = ExtractErrorMessage(exec_rsp);
                if (error_msg) {
                    *error_msg = "node " + node_id + " execute failed";
                    if (!err.empty()) *error_msg += ": " + err;
                }
                return rc;
            }

            if (!ExtractRuntimeTaskId(exec_rsp, node_runtime_task_id) || node_runtime_task_id->empty()) {
                if (error_msg) *error_msg = "node " + node_id + " execute missing runtime_task_id";
                return EIO;
            }
            {
                std::lock_guard<std::mutex> lock(stream_group_nodes_mu_);
                stream_group_node_owners_[*node_runtime_task_id] = runtime_task_id;
            }
            return 0;
        },
        [this](const std::string& node_runtime_task_id, TaskSnapshot* snapshot_out) -> int {
            return QueryStreamTaskSnapshotByRuntimeId(node_runtime_task_id, snapshot_out);
        },
        [this](const std::string& node_runtime_task_id) {
            RequestStopStreamTaskByRuntimeId(node_runtime_task_id);
        },
        stop_group_hubs);

    {
        std::lock_guard<std::mutex> lock(stream_task_groups_mu_);
        stream_task_groups_[runtime_task_id] = group;
    }
    {
        std::unordered_map<std::string, GroupNodeResolvedSourceMeta> node_source_snapshot;
        node_source_snapshot.reserve(plans.size());
        for (const auto& plan : plans) {
            auto it = node_resolved.find(plan.id);
            if (it == node_resolved.end()) continue;
            GroupNodeResolvedSourceMeta meta;
            meta.sources = it->second.resolved_sources;
            meta.expand_rule = it->second.expand_rule;
            node_source_snapshot.emplace(plan.id, std::move(meta));
        }
        std::lock_guard<std::mutex> lock(stream_group_node_sources_mu_);
        stream_group_node_sources_[runtime_task_id] = std::move(node_source_snapshot);
    }
    if (!share_set_runtimes.empty()) {
        std::lock_guard<std::mutex> lock(stream_group_share_sets_mu_);
        stream_group_share_sets_[runtime_task_id] = share_set_runtimes;
    }
    {
        std::lock_guard<std::mutex> lock(stream_group_share_set_snapshots_mu_);
        stream_group_share_set_snapshots_.erase(runtime_task_id);
    }

    std::string start_err;
    const int start_rc = group->Start(&start_err);
    if (start_rc != 0) {
        {
            std::lock_guard<std::mutex> lock(stream_task_groups_mu_);
            stream_task_groups_.erase(runtime_task_id);
        }
        {
            std::lock_guard<std::mutex> lock(stream_group_share_sets_mu_);
            stream_group_share_sets_.erase(runtime_task_id);
        }
        {
            std::lock_guard<std::mutex> lock(stream_group_node_sources_mu_);
            stream_group_node_sources_.erase(runtime_task_id);
        }
        cleanup_local_resources();
        rsp = MakeErrorJsonLocal("start stream group failed: " +
                                 (start_err.empty() ? std::to_string(start_rc) : start_err));
        return error::INTERNAL_ERROR;
    }

    for (auto& ss_runtime : share_set_runtimes) {
        const int64_t deadline_ms = CurrentTimeMsLocal() + static_cast<int64_t>(share_set_ready_timeout_s) * 1000;
        bool ready = false;
        while (CurrentTimeMsLocal() < deadline_ms) {
            auto snapshot = group->Snapshot();
            if (snapshot.status == StreamGroupStatus::kFailed ||
                snapshot.status == StreamGroupStatus::kCancelled ||
                snapshot.status == StreamGroupStatus::kStopped) {
                break;
            }

            bool all_started = true;
            bool has_terminal_fail = false;
            for (const auto& member : ss_runtime.members) {
                bool found = false;
                for (const auto& node : snapshot.nodes) {
                    if (node.node_id != member) continue;
                    found = true;
                    if (node.status == GroupNodeStatus::kPending ||
                        node.status == GroupNodeStatus::kReady) {
                        all_started = false;
                    }
                    if (node.status == GroupNodeStatus::kFailed ||
                        node.status == GroupNodeStatus::kCancelled ||
                        node.status == GroupNodeStatus::kSkipped) {
                        has_terminal_fail = true;
                    }
                    break;
                }
                if (!found) all_started = false;
            }
            if (has_terminal_fail) break;
            if (all_started) {
                ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        if (!ready) {
            group->MarkExternalFailed(
                ETIMEDOUT,
                "share_set ready timeout: " + ss_runtime.id +
                    ", timeout_s=" + std::to_string(share_set_ready_timeout_s),
                "STREAM_GROUP_SHARE_SET_READY_TIMEOUT");
            break;
        }

        std::string hub_err;
        const int hub_rc = ss_runtime.hub ? ss_runtime.hub->Start(&hub_err) : EINVAL;
        if (hub_rc != 0) {
            group->MarkExternalFailed(
                hub_rc,
                "share_set start failed: " + ss_runtime.id +
                    (hub_err.empty() ? "" : (", " + hub_err)),
                "STREAM_GROUP_SHARE_SET_START_FAILED");
            break;
        }
    }

    release_group_lease_on_fail = false;
    group_lease_guard.reset();

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("status");
    w.String("submitted");
    w.Key("task_id");
    w.String(runtime_task_id.c_str());
    w.Key("runtime_task_id");
    w.String(runtime_task_id.c_str());
    w.Key("runtime_kind");
    w.String("group");
    w.Key("group_mode");
    w.String("dag");
    w.Key("node_count");
    w.Uint(static_cast<unsigned>(plans.size()));
    w.Key("share_set_count");
    w.Uint(static_cast<unsigned>(share_set_plans.size()));
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

}  // namespace scheduler
}  // namespace flowsql
