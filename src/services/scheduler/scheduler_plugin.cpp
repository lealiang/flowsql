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
#include "framework/core/plugin_registry.h"
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

int SchedulerPlugin::Load() {
    registry_ = PluginRegistry::Instance();
    if (!registry_) {
        printf("SchedulerPlugin::Load: PluginRegistry not available\n");
        return -1;
    }
    printf("SchedulerPlugin::Load: host=%s, port=%d\n", host_.c_str(), port_);
    return 0;
}

int SchedulerPlugin::Unload() {
    return 0;
}

// --- IPlugin::Start ---
int SchedulerPlugin::Start() {
    // 创建预填测试数据（通道注册在 Scheduler 进程中）
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
    registry_->Register(IID_CHANNEL, "test.data", ch);
    registry_->Register(IID_DATAFRAME_CHANNEL, "test.data", ch);

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
    printf("SchedulerPlugin::Stop: done\n");
    return 0;
}

// --- 路由注册 ---
void SchedulerPlugin::RegisterRoutes() {
    // CORS 预检
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
}

// --- 通道查找辅助 ---
IChannel* SchedulerPlugin::FindChannel(const std::string& name) {
    // 先按完整 key 查找
    IChannel* ch = registry_->Get<IChannel>(IID_CHANNEL, name);
    if (ch) return ch;

    // 按 catelog.name 格式拆分后查找
    auto dot = name.find('.');
    if (dot != std::string::npos) {
        std::string key = name.substr(0, dot) + "." + name.substr(dot + 1);
        ch = registry_->Get<IChannel>(IID_CHANNEL, key);
        if (ch) return ch;
    }

    // 模糊匹配：遍历所有通道
    registry_->Traverse<IChannel>(IID_CHANNEL, [&](IChannel* c) {
        if (std::string(c->Name()) == name ||
            (std::string(c->Catelog()) + "." + c->Name()) == name) {
            ch = c;
        }
    });
    return ch;
}

// --- 构建 Database 查询语句 ---
static std::string BuildQuery(const std::string& source_name, const SqlStatement& stmt) {
    // 从通道名提取表名（catelog.table → table）
    std::string table = source_name;
    auto dot = table.find('.');
    if (dot != std::string::npos) {
        table = table.substr(dot + 1);
    }

    // 构建 SELECT 子句
    std::string select_clause = "*";
    if (!stmt.columns.empty()) {
        select_clause.clear();
        for (size_t i = 0; i < stmt.columns.size(); ++i) {
            if (i > 0) select_clause += ", ";
            select_clause += stmt.columns[i];
        }
    }

    std::string query = "SELECT " + select_clause + " FROM " + table;

    // WITH 参数中的 where 条件
    auto it = stmt.with_params.find("where");
    if (it != stmt.with_params.end()) {
        query += " WHERE " + it->second;
    }

    return query;
}

// --- 从 sink 通道名提取目标表名 ---
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
        // DataFrame → DataFrame：直接搬运
        auto* src = dynamic_cast<IDataFrameChannel*>(source);
        auto* dst = dynamic_cast<IDataFrameChannel*>(sink);
        if (!src || !dst) return -1;
        return ChannelAdapter::CopyDataFrame(src, dst);
    }

    if (source_type == "dataframe" && sink_type == "database") {
        // DataFrame → Database：写入
        auto* src = dynamic_cast<IDataFrameChannel*>(source);
        auto* dst = dynamic_cast<IDatabaseChannel*>(sink);
        if (!src || !dst) return -1;
        std::string table = ExtractTableName(stmt.dest);
        return ChannelAdapter::WriteFromDataFrame(src, dst, table.c_str());
    }

    if (source_type == "database" && sink_type == "dataframe") {
        // Database → DataFrame：读取
        auto* src = dynamic_cast<IDatabaseChannel*>(source);
        auto* dst = dynamic_cast<IDataFrameChannel*>(sink);
        if (!src || !dst) return -1;
        std::string query = BuildQuery(stmt.source, stmt);
        return ChannelAdapter::ReadToDataFrame(src, query.c_str(), dst);
    }

    if (source_type == "database" && sink_type == "database") {
        // Database → Database：通过临时 DataFrame 中转
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

    // source 是 Database → 先读取到临时 DataFrame
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

    // sink 是 Database → 算子输出到临时 DataFrame，之后再写入数据库
    if (sink_type == "database") {
        tmp_out = std::make_shared<DataFrameChannel>("_adapter", "out");
        tmp_out->Open();
        actual_sink = tmp_out.get();
    }

    // 执行 Pipeline：算子看到的永远是 IDataFrameChannel
    auto pipeline = PipelineBuilder()
                        .SetSource(actual_source)
                        .SetOperator(op)
                        .SetSink(actual_sink)
                        .Build();
    pipeline->Run();

    if (pipeline->State() == PipelineState::FAILED) {
        return -1;
    }

    // 如果 sink 是 Database，将临时 DataFrame 写入数据库
    if (sink_type == "database" && tmp_out) {
        auto* db_sink = dynamic_cast<IDatabaseChannel*>(sink);
        if (!db_sink) return -1;

        std::string table = ExtractTableName(stmt.dest);
        int rc = ChannelAdapter::WriteFromDataFrame(tmp_out.get(), db_sink, table.c_str());
        if (rc != 0) return rc;
    }

    return 0;
}

// --- HandleExecute: 类型感知的 SQL 执行逻辑 ---
void SchedulerPlugin::HandleExecute(const httplib::Request& req, httplib::Response& res) {
    SetCorsHeaders(res);

    // 解析请求 JSON: {"sql": "SELECT * FROM ..."}
    rapidjson::Document doc;
    doc.Parse(req.body.c_str());
    if (doc.HasParseError() || !doc.HasMember("sql") || !doc["sql"].IsString()) {
        res.status = 400;
        res.set_content(MakeErrorJson("invalid request, expected {\"sql\":\"...\"}"), "application/json");
        return;
    }
    std::string sql_text = doc["sql"].GetString();

    // 解析 SQL
    SqlParser parser;
    auto stmt = parser.Parse(sql_text);
    if (!stmt.error.empty()) {
        res.status = 400;
        res.set_content(MakeErrorJson(stmt.error), "application/json");
        return;
    }

    // 查找 source channel
    IChannel* source = FindChannel(stmt.source);
    if (!source) {
        res.status = 400;
        res.set_content(MakeErrorJson("source channel not found: " + stmt.source), "application/json");
        return;
    }

    // 查找 operator（可选）
    IOperator* op = nullptr;
    if (stmt.HasOperator()) {
        op = registry_->Get<IOperator>(IID_OPERATOR, stmt.op_catelog + "." + stmt.op_name);
        if (!op) {
            registry_->Traverse<IOperator>(IID_OPERATOR, [&](IOperator* o) {
                if (o->Catelog() == stmt.op_catelog && o->Name() == stmt.op_name) op = o;
            });
        }
        if (!op) {
            res.status = 400;
            res.set_content(MakeErrorJson("operator not found: " + stmt.op_catelog + "." + stmt.op_name),
                            "application/json");
            return;
        }
    }

    try {
        // 传递 WITH 参数给算子
        if (op) {
            for (auto& [k, v] : stmt.with_params) {
                op->Configure(k.c_str(), v.c_str());
            }
        }

        // 准备 sink channel
        std::shared_ptr<DataFrameChannel> temp_sink;
        IChannel* sink = nullptr;

        if (!stmt.dest.empty()) {
            sink = FindChannel(stmt.dest);
            if (!sink) {
                // 目标通道不存在，创建临时 DataFrameChannel
                temp_sink = std::make_shared<DataFrameChannel>("result", stmt.dest);
                temp_sink->Open();
                registry_->Register(IID_CHANNEL, "result." + stmt.dest, temp_sink);
                registry_->Register(IID_DATAFRAME_CHANNEL, "result." + stmt.dest, temp_sink);
                sink = temp_sink.get();
            }
        } else {
            // 无 INTO，创建临时 sink
            temp_sink = std::make_shared<DataFrameChannel>("_temp", "sink");
            temp_sink->Open();
            sink = temp_sink.get();
        }

        // 获取通道类型
        std::string source_type(source->Type());
        std::string sink_type(sink->Type());

        int rc = 0;

        if (!op) {
            // 无算子：纯数据搬运
            rc = ExecuteTransfer(source, sink, source_type, sink_type, stmt);
        } else {
            // 有算子：需要确保算子看到的是 IDataFrameChannel
            rc = ExecuteWithOperator(source, sink, op, source_type, sink_type, stmt);
        }

        if (rc != 0) {
            std::string err = op ? op->LastError() : "";
            if (err.empty()) err = "execution failed";
            res.status = 500;
            res.set_content(MakeErrorJson(err), "application/json");
            return;
        }

        // 从 sink 读取结果
        auto* df_sink = dynamic_cast<IDataFrameChannel*>(sink);
        DataFrame result;
        std::string result_json = "[]";
        if (df_sink && df_sink->Read(&result) == 0 && result.RowCount() > 0) {
            result_json = result.ToJson();
        }

        // 返回执行结果
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

// --- HandleGetChannels: 返回已注册通道列表 ---
void SchedulerPlugin::HandleGetChannels(const httplib::Request&, httplib::Response& res) {
    SetCorsHeaders(res);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();
    registry_->Traverse<IChannel>(IID_CHANNEL, [&w](IChannel* ch) {
        w.StartObject();
        w.Key("catelog");
        w.String(ch->Catelog());
        w.Key("name");
        w.String(ch->Name());
        w.Key("type");
        w.String(ch->Type());
        w.Key("schema");
        w.String(ch->Schema());
        w.EndObject();
    });
    w.EndArray();
    res.set_content(buf.GetString(), "application/json");
}

// --- HandleGetOperators: 返回已注册算子列表 ---
void SchedulerPlugin::HandleGetOperators(const httplib::Request&, httplib::Response& res) {
    SetCorsHeaders(res);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();
    registry_->Traverse<IOperator>(IID_OPERATOR, [&w](IOperator* op) {
        w.StartObject();
        w.Key("catelog");
        w.String(op->Catelog().c_str());
        w.Key("name");
        w.String(op->Name().c_str());
        w.Key("description");
        w.String(op->Description().c_str());
        std::string pos = (op->Position() == OperatorPosition::STORAGE) ? "STORAGE" : "DATA";
        w.Key("position");
        w.String(pos.c_str());
        w.EndObject();
    });
    w.EndArray();
    res.set_content(buf.GetString(), "application/json");
}

}  // namespace scheduler
}  // namespace flowsql
