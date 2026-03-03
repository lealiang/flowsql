#include "scheduler_plugin.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "framework/core/channel_adapter.h"
#include "framework/core/dataframe.h"
#include "framework/core/dataframe_channel.h"
#include "framework/core/pipeline.h"
#include "framework/core/sql_parser.h"
#include "framework/interfaces/ichannel.h"
#include "framework/interfaces/idatabase_channel.h"
#include "framework/interfaces/idataframe_channel.h"
#include "framework/interfaces/ioperator.h"

namespace flowsql {
namespace scheduler {

// --- JSON 辅助 ---
static void SetCorsHeaders(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
}

static std::string MakeErrorJson(const std::string& error) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error.c_str());
    w.EndObject();
    return buf.GetString();
}

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

        pos = (end < opts.size()) ? end + 1 : opts.size();
    }
    return 0;
}

int SchedulerPlugin::Load(IQuerier* querier) {
    querier_ = querier;
    printf("SchedulerPlugin::Load: host=%s, port=%d\n", host_.c_str(), port_);
    return 0;
}

int SchedulerPlugin::Unload() {
    return 0;
}

// --- 通道管理 ---
void SchedulerPlugin::RegisterChannel(const std::string& key, std::shared_ptr<IChannel> ch) {
    channels_[key] = std::move(ch);
}

// --- IPlugin::Start ---
int SchedulerPlugin::Start() {
    // 创建预填测试数据
    auto ch = std::make_shared<DataFrameChannel>("test", "data");
    ch->Open();

    DataFrame df;
    df.SetSchema({
        {"src_ip", DataType::STRING, 0, "源IP"},
        {"dst_ip", DataType::STRING, 0, "目的IP"},
        {"src_port", DataType::UINT32, 0, "源端口"},
        {"dst_port", DataType::UINT32, 0, "目的端口"},
        {"protocol", DataType::STRING, 0, "协议"},
        {"bytes_sent", DataType::UINT64, 0, "发送字节"},
        {"bytes_recv", DataType::UINT64, 0, "接收字节"},
    });

    df.AppendRow({std::string("192.168.1.10"), std::string("10.0.0.1"), uint32_t(52341), uint32_t(80),
                  std::string("HTTP"), uint64_t(1024), uint64_t(4096)});
    df.AppendRow({std::string("192.168.1.10"), std::string("8.8.8.8"), uint32_t(53421), uint32_t(53),
                  std::string("DNS"), uint64_t(64), uint64_t(128)});
    df.AppendRow({std::string("192.168.1.20"), std::string("172.16.0.5"), uint32_t(44312), uint32_t(443),
                  std::string("HTTPS"), uint64_t(2048), uint64_t(8192)});

    ch->Write(&df);
    RegisterChannel("test.data", ch);

    RegisterRoutes();

    server_thread_ = std::thread([this]() {
        printf("SchedulerPlugin: listening on %s:%d\n", host_.c_str(), port_);
        if (!server_.listen(host_, port_)) {
            printf("SchedulerPlugin: failed to start HTTP server\n");
        }
    });

    return 0;
}

int SchedulerPlugin::Stop() {
    server_.stop();
    if (server_thread_.joinable()) server_thread_.join();
    channels_.clear();
    printf("SchedulerPlugin::Stop: done\n");
    return 0;
}

// --- 路由注册 ---
void SchedulerPlugin::RegisterRoutes() {
    server_.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
    });

    server_.Post("/execute", [this](const httplib::Request& req, httplib::Response& res) {
        HandleExecute(req, res);
    });

    server_.Get("/channels", [this](const httplib::Request& req, httplib::Response& res) {
        HandleGetChannels(req, res);
    });

    server_.Get("/operators", [this](const httplib::Request& req, httplib::Response& res) {
        HandleGetOperators(req, res);
    });

    server_.Post("/refresh-operators", [this](const httplib::Request&, httplib::Response& res) {
        HandleRefreshOperators(res);
    });
}

// --- 通道查找辅助 ---
IChannel* SchedulerPlugin::FindChannel(const std::string& name) {
    // 先在内部通道表中查找
    auto it = channels_.find(name);
    if (it != channels_.end()) return it->second.get();

    // 按 catelog.name 格式拆分后查找
    auto dot = name.find('.');
    if (dot != std::string::npos) {
        std::string key = name.substr(0, dot) + "." + name.substr(dot + 1);
        it = channels_.find(key);
        if (it != channels_.end()) return it->second.get();
    }

    // 通过 IQuerier 遍历静态注册的通道
    IChannel* found = nullptr;
    if (querier_) {
        querier_->Traverse(IID_CHANNEL, [&](void* p) -> int {
            auto* c = static_cast<IChannel*>(p);
            std::string full_name = std::string(c->Catelog()) + "." + c->Name();
            if (full_name == name || std::string(c->Name()) == name) {
                found = c;
                return -1;  // 找到了，停止遍历
            }
            return 0;
        });
    }

    // 模糊匹配内部通道表
    if (!found) {
        for (auto& [k, v] : channels_) {
            auto* c = v.get();
            if (std::string(c->Name()) == name ||
                (std::string(c->Catelog()) + "." + c->Name()) == name) {
                found = c;
                break;
            }
        }
    }
    return found;
}

// --- 算子查找 ---
// 先查 C++ 静态算子（IQuerier），再查 Python 算子（IBridge）
std::shared_ptr<IOperator> SchedulerPlugin::FindOperator(const std::string& catelog, const std::string& name) {
    if (!querier_) return nullptr;

    // 1. 先查 C++ 静态算子
    IOperator* found = nullptr;
    querier_->Traverse(IID_OPERATOR, [&](void* p) -> int {
        auto* op = static_cast<IOperator*>(p);
        if (op->Catelog() == catelog && op->Name() == name) {
            found = op;
            return -1;
        }
        return 0;
    });
    // C++ 算子由 PluginLoader 管理生命周期，用空 deleter 包装
    if (found) return std::shared_ptr<IOperator>(found, [](IOperator*) {});

    // 2. 再查 Python 算子（通过 IBridge）
    auto* bridge = static_cast<IBridge*>(querier_->First(IID_BRIDGE));
    if (bridge) return bridge->FindOperator(catelog, name);
    return nullptr;
}

// --- 构建 Database 查询语句 ---
static std::string BuildQuery(const std::string& source_name, const SqlStatement& stmt) {
    std::string table = source_name;
    auto dot = table.find('.');
    if (dot != std::string::npos) {
        table = table.substr(dot + 1);
    }

    std::string select_clause = "*";
    if (!stmt.columns.empty()) {
        select_clause.clear();
        for (size_t i = 0; i < stmt.columns.size(); ++i) {
            if (i > 0) select_clause += ", ";
            select_clause += stmt.columns[i];
        }
    }

    std::string query = "SELECT " + select_clause + " FROM " + table;

    auto it = stmt.with_params.find("where");
    if (it != stmt.with_params.end()) {
        query += " WHERE " + it->second;
    }

    return query;
}

static std::string ExtractTableName(const std::string& dest_name) {
    auto dot = dest_name.find('.');
    if (dot != std::string::npos) {
        return dest_name.substr(dot + 1);
    }
    return dest_name;
}

// --- 无算子：纯数据搬运 ---
int SchedulerPlugin::ExecuteTransfer(IChannel* source, IChannel* sink,
                                      const std::string& source_type,
                                      const std::string& sink_type,
                                      const SqlStatement& stmt) {
    if (source_type == "dataframe" && sink_type == "dataframe") {
        auto* src = dynamic_cast<IDataFrameChannel*>(source);
        auto* dst = dynamic_cast<IDataFrameChannel*>(sink);
        if (!src || !dst) return -1;
        return ChannelAdapter::CopyDataFrame(src, dst);
    }

    if (source_type == "dataframe" && sink_type == "database") {
        auto* src = dynamic_cast<IDataFrameChannel*>(source);
        auto* dst = dynamic_cast<IDatabaseChannel*>(sink);
        if (!src || !dst) return -1;
        std::string table = ExtractTableName(stmt.dest);
        return ChannelAdapter::WriteFromDataFrame(src, dst, table.c_str());
    }

    if (source_type == "database" && sink_type == "dataframe") {
        auto* src = dynamic_cast<IDatabaseChannel*>(source);
        auto* dst = dynamic_cast<IDataFrameChannel*>(sink);
        if (!src || !dst) return -1;
        std::string query = BuildQuery(stmt.source, stmt);
        return ChannelAdapter::ReadToDataFrame(src, query.c_str(), dst);
    }

    if (source_type == "database" && sink_type == "database") {
        auto* src = dynamic_cast<IDatabaseChannel*>(source);
        auto* dst = dynamic_cast<IDatabaseChannel*>(sink);
        if (!src || !dst) return -1;

        auto tmp = std::make_shared<DataFrameChannel>("_adapter", "tmp");
        tmp->Open();

        std::string query = BuildQuery(stmt.source, stmt);
        int rc = ChannelAdapter::ReadToDataFrame(src, query.c_str(), tmp.get());
        if (rc != 0) return rc;

        std::string table = ExtractTableName(stmt.dest);
        return ChannelAdapter::WriteFromDataFrame(tmp.get(), dst, table.c_str());
    }

    printf("SchedulerPlugin: unsupported transfer: %s → %s\n",
           source_type.c_str(), sink_type.c_str());
    return -1;
}

// --- 有算子：自动适配通道类型 ---
int SchedulerPlugin::ExecuteWithOperator(IChannel* source, IChannel* sink,
                                          IOperator* op,
                                          const std::string& source_type,
                                          const std::string& sink_type,
                                          const SqlStatement& stmt) {
    IChannel* actual_source = source;
    IChannel* actual_sink = sink;
    std::shared_ptr<DataFrameChannel> tmp_in, tmp_out;

    if (source_type == "database") {
        auto* db_src = dynamic_cast<IDatabaseChannel*>(source);
        if (!db_src) return -1;

        tmp_in = std::make_shared<DataFrameChannel>("_adapter", "in");
        tmp_in->Open();

        std::string query = BuildQuery(stmt.source, stmt);
        int rc = ChannelAdapter::ReadToDataFrame(db_src, query.c_str(), tmp_in.get());
        if (rc != 0) return rc;

        actual_source = tmp_in.get();
    }

    if (sink_type == "database") {
        tmp_out = std::make_shared<DataFrameChannel>("_adapter", "out");
        tmp_out->Open();
        actual_sink = tmp_out.get();
    }

    auto pipeline = PipelineBuilder()
                        .SetSource(actual_source)
                        .SetOperator(op)
                        .SetSink(actual_sink)
                        .Build();
    pipeline->Run();

    if (pipeline->State() == PipelineState::FAILED) {
        return -1;
    }

    if (sink_type == "database" && tmp_out) {
        auto* db_sink = dynamic_cast<IDatabaseChannel*>(sink);
        if (!db_sink) return -1;

        std::string table = ExtractTableName(stmt.dest);
        int rc = ChannelAdapter::WriteFromDataFrame(tmp_out.get(), db_sink, table.c_str());
        if (rc != 0) return rc;
    }

    return 0;
}

// --- HandleExecute ---
void SchedulerPlugin::HandleExecute(const httplib::Request& req, httplib::Response& res) {
    SetCorsHeaders(res);

    rapidjson::Document doc;
    doc.Parse(req.body.c_str());
    if (doc.HasParseError() || !doc.HasMember("sql") || !doc["sql"].IsString()) {
        res.status = 400;
        res.set_content(MakeErrorJson("invalid request, expected {\"sql\":\"...\"}"), "application/json");
        return;
    }
    std::string sql_text = doc["sql"].GetString();

    SqlParser parser;
    auto stmt = parser.Parse(sql_text);
    if (!stmt.error.empty()) {
        res.status = 400;
        res.set_content(MakeErrorJson(stmt.error), "application/json");
        return;
    }

    IChannel* source = FindChannel(stmt.source);
    if (!source) {
        res.status = 400;
        res.set_content(MakeErrorJson("source channel not found: " + stmt.source), "application/json");
        return;
    }

    IOperator* op = nullptr;
    std::shared_ptr<IOperator> op_holder;  // 持有 shared_ptr 保证算子生命周期
    if (stmt.HasOperator()) {
        op_holder = FindOperator(stmt.op_catelog, stmt.op_name);
        if (!op_holder) {
            res.status = 400;
            res.set_content(MakeErrorJson("operator not found: " + stmt.op_catelog + "." + stmt.op_name),
                            "application/json");
            return;
        }
        op = op_holder.get();
    }

    try {
        if (op) {
            for (auto& [k, v] : stmt.with_params) {
                op->Configure(k.c_str(), v.c_str());
            }
        }

        std::shared_ptr<DataFrameChannel> temp_sink;
        IChannel* sink = nullptr;

        if (!stmt.dest.empty()) {
            sink = FindChannel(stmt.dest);
            if (!sink) {
                temp_sink = std::make_shared<DataFrameChannel>("result", stmt.dest);
                temp_sink->Open();
                RegisterChannel("result." + stmt.dest, temp_sink);
                sink = temp_sink.get();
            }
        } else {
            temp_sink = std::make_shared<DataFrameChannel>("_temp", "sink");
            temp_sink->Open();
            sink = temp_sink.get();
        }

        std::string source_type(source->Type());
        std::string sink_type(sink->Type());

        int rc = 0;

        if (!op) {
            rc = ExecuteTransfer(source, sink, source_type, sink_type, stmt);
        } else {
            rc = ExecuteWithOperator(source, sink, op, source_type, sink_type, stmt);
        }

        if (rc != 0) {
            std::string err = op ? op->LastError() : "";
            if (err.empty()) err = "execution failed";
            res.status = 500;
            res.set_content(MakeErrorJson(err), "application/json");
            return;
        }

        auto* df_sink = dynamic_cast<IDataFrameChannel*>(sink);
        DataFrame result;
        std::string result_json = "[]";
        if (df_sink && df_sink->Read(&result) == 0 && result.RowCount() > 0) {
            result_json = result.ToJson();
        }

        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("status");
        w.String("completed");
        w.Key("rows");
        w.Int(result.RowCount());
        w.Key("data");
        w.RawValue(result_json.c_str(), result_json.size(), rapidjson::kArrayType);
        w.EndObject();
        res.set_content(buf.GetString(), "application/json");

    } catch (const std::exception& e) {
        std::string err = std::string("internal error: ") + e.what();
        printf("SchedulerPlugin::HandleExecute: exception: %s\n", err.c_str());
        res.status = 500;
        res.set_content(MakeErrorJson(err), "application/json");
    } catch (...) {
        printf("SchedulerPlugin::HandleExecute: unknown exception\n");
        res.status = 500;
        res.set_content(MakeErrorJson("internal error: unknown exception"), "application/json");
    }
}

// --- HandleGetChannels ---
void SchedulerPlugin::HandleGetChannels(const httplib::Request&, httplib::Response& res) {
    SetCorsHeaders(res);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();

    // 内部通道表
    for (auto& [key, ch_ptr] : channels_) {
        auto* ch = ch_ptr.get();
        w.StartObject();
        w.Key("catelog"); w.String(ch->Catelog());
        w.Key("name"); w.String(ch->Name());
        w.Key("type"); w.String(ch->Type());
        w.Key("schema"); w.String(ch->Schema());
        w.EndObject();
    }

    // 静态注册的通道（通过 IQuerier）
    if (querier_) {
        querier_->Traverse(IID_CHANNEL, [&w](void* p) -> int {
            auto* ch = static_cast<IChannel*>(p);
            w.StartObject();
            w.Key("catelog"); w.String(ch->Catelog());
            w.Key("name"); w.String(ch->Name());
            w.Key("type"); w.String(ch->Type());
            w.Key("schema"); w.String(ch->Schema());
            w.EndObject();
            return 0;
        });
    }

    w.EndArray();
    res.set_content(buf.GetString(), "application/json");
}

// --- HandleGetOperators ---
void SchedulerPlugin::HandleGetOperators(const httplib::Request&, httplib::Response& res) {
    SetCorsHeaders(res);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();

    if (querier_) {
        querier_->Traverse(IID_OPERATOR, [&w](void* p) -> int {
            auto* op = static_cast<IOperator*>(p);
            w.StartObject();
            w.Key("catelog"); w.String(op->Catelog().c_str());
            w.Key("name"); w.String(op->Name().c_str());
            w.Key("description"); w.String(op->Description().c_str());
            std::string pos = (op->Position() == OperatorPosition::STORAGE) ? "STORAGE" : "DATA";
            w.Key("position"); w.String(pos.c_str());
            w.EndObject();
            return 0;
        });

        // Python 算子（通过 IBridge 遍历）
        auto* bridge = static_cast<IBridge*>(querier_->First(IID_BRIDGE));
        if (bridge) {
            bridge->TraverseOperators([&w](IOperator* op) -> int {
                w.StartObject();
                w.Key("catelog"); w.String(op->Catelog().c_str());
                w.Key("name"); w.String(op->Name().c_str());
                w.Key("description"); w.String(op->Description().c_str());
                std::string pos = (op->Position() == OperatorPosition::STORAGE) ? "STORAGE" : "DATA";
                w.Key("position"); w.String(pos.c_str());
                w.EndObject();
                return 0;
            });
        }
    }

    w.EndArray();
    res.set_content(buf.GetString(), "application/json");
}

// --- HandleRefreshOperators ---
void SchedulerPlugin::HandleRefreshOperators(httplib::Response& res) {
    SetCorsHeaders(res);
    auto* bridge = static_cast<IBridge*>(querier_->First(IID_BRIDGE));
    if (bridge) {
        int rc = bridge->Refresh();
        if (rc == 0) {
            res.set_content(R"({"status":"refreshed"})", "application/json");
        } else {
            res.status = 500;
            res.set_content(R"({"error":"refresh failed"})", "application/json");
        }
    } else {
        res.status = 404;
        res.set_content(R"({"error":"bridge not available"})", "application/json");
    }
}

}  // namespace scheduler
}  // namespace flowsql

