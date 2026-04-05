#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <common/error_code.h>
#include <common/loader.hpp>
#include <framework/core/dataframe.h>
#include <framework/core/stream_channel_adapter.h>
#include <framework/core/dataframe_channel.h>
#include <framework/interfaces/idatabase_channel.h>
#include <framework/interfaces/idatabase_factory.h>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/idataframe_channel.h>
#include <framework/interfaces/ioperator.h>
#include <framework/interfaces/ioperator_registry.h>
#include <framework/interfaces/irouter_handle.h>
#include <framework/interfaces/istream_channel.h>
#include <framework/interfaces/istream_factory.h>
#include <framework/interfaces/istream_operator.h>

using namespace flowsql;

#define ASSERT_TRUE(expr)                                                                   \
    do {                                                                                    \
        if (!(expr)) {                                                                      \
            std::printf("[FAIL] %s:%d %s\n", __FILE__, __LINE__, #expr);                   \
            std::fflush(stdout);                                                            \
            assert(false);                                                                  \
        }                                                                                   \
    } while (0)

#define ASSERT_EQ(a, b)                                                                     \
    do {                                                                                    \
        auto _a = (a);                                                                      \
        auto _b = (b);                                                                      \
        if (!(_a == _b)) {                                                                  \
            std::printf("[FAIL] %s:%d %s != %s\n", __FILE__, __LINE__, #a, #b);            \
            std::fflush(stdout);                                                            \
            assert(false);                                                                  \
        }                                                                                   \
    } while (0)

static std::shared_ptr<arrow::Buffer> SerializeBatch(const std::shared_ptr<arrow::RecordBatch>& batch) {
    auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
    auto ipc_w = arrow::ipc::MakeStreamWriter(sink, batch->schema()).ValueOrDie();
    (void)ipc_w->WriteRecordBatch(*batch);
    (void)ipc_w->Close();
    return sink->Finish().ValueOrDie();
}

static int64_t CountRowsInIpc(const uint8_t* data, size_t len) {
    auto buf = std::make_shared<arrow::Buffer>(data, len);
    auto reader = arrow::ipc::RecordBatchStreamReader::Open(std::make_shared<arrow::io::BufferReader>(buf)).ValueOrDie();
    int64_t rows = 0;
    while (true) {
        auto batch = reader->Next().ValueOrDie();
        if (!batch) break;
        rows += batch->num_rows();
    }
    return rows;
}

static std::string MakeReq(const std::string& sql) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("sql");
    w.String(sql.c_str());
    w.EndObject();
    return buf.GetString();
}

static std::string MakeTaskReq(const std::string& task_id) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("task_id");
    w.String(task_id.c_str());
    w.EndObject();
    return buf.GetString();
}

static std::shared_ptr<arrow::RecordBatch> MakeStreamBatch(int64_t base, int64_t rows = 1) {
    auto schema = arrow::schema({arrow::field("v", arrow::int64())});
    arrow::Int64Builder b;
    for (int64_t i = 0; i < rows; ++i) {
        (void)b.Append(base + i);
    }
    auto arr = b.Finish().ValueOrDie();
    return arrow::RecordBatch::Make(schema, rows, {arr});
}

static std::string ParseStatus(const std::string& rsp) {
    rapidjson::Document d;
    d.Parse(rsp.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("status") || !d["status"].IsString()) {
        return "";
    }
    return d["status"].GetString();
}

static std::string ParseTaskId(const std::string& rsp) {
    rapidjson::Document d;
    d.Parse(rsp.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("runtime_task_id") || !d["runtime_task_id"].IsString()) {
        return "";
    }
    return d["runtime_task_id"].GetString();
}

static int ParseShardCount(const std::string& rsp) {
    rapidjson::Document d;
    d.Parse(rsp.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("shard_count") || !d["shard_count"].IsUint()) {
        return -1;
    }
    return static_cast<int>(d["shard_count"].GetUint());
}

static bool IsValidStreamChannelList(const std::string& rsp) {
    rapidjson::Document d;
    d.Parse(rsp.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("channels") || !d["channels"].IsArray()) {
        return false;
    }
    const auto& arr = d["channels"].GetArray();
    for (auto it = arr.Begin(); it != arr.End(); ++it) {
        if (!it->IsObject()) return false;
        if (!it->HasMember("type") || !(*it)["type"].IsString()) return false;
        if (!it->HasMember("name") || !(*it)["name"].IsString()) return false;
        if (!it->HasMember("status") || !(*it)["status"].IsString()) return false;
    }
    return true;
}

class ParallelPassthroughStreamOperator final : public IOperator, public IStreamOperator {
 public:
    std::string Category() override { return "builtin"; }
    std::string Name() override { return "passthrough_stream"; }
    std::string Description() override { return "test parallel passthrough stream op"; }
    OperatorPosition Position() override { return OperatorPosition::DATA; }

    int Work(IChannel*, IChannel*) override { return -1; }
    int Configure(const char*, const char*) override { return 0; }

    int Init(const char*, const StreamSinkContext& sink_ctx) override {
        last_error_.clear();
        output_.reset();
        if (!sink_ctx.sink_channel) {
            last_error_ = "sink channel is null";
            return -1;
        }
        if (sink_ctx.sink_type == ChannelType::kStream) {
            auto* out = dynamic_cast<IStreamChannel*>(sink_ctx.sink_channel);
            if (!out) {
                last_error_ = "stream sink cast failed";
                return -1;
            }
            output_ = std::shared_ptr<IStreamChannel>(out, [](IStreamChannel*) {});
            return 0;
        }
        if (sink_ctx.sink_type == ChannelType::kDataFrame) {
            auto* appendable = dynamic_cast<IAppendableDataFrameChannel*>(sink_ctx.sink_channel);
            if (!appendable) {
                last_error_ = "dataframe sink must be appendable";
                return -1;
            }
            output_ = StreamChannelAdapter::MakeDataFrameAppend(
                "stream_adapter",
                sink_ctx.into_raw,
                std::shared_ptr<IAppendableDataFrameChannel>(appendable, [](IAppendableDataFrameChannel*) {}));
            return output_ ? 0 : -1;
        }
        if (sink_ctx.sink_type == ChannelType::kDatabase) {
            auto* db = dynamic_cast<IDatabaseChannel*>(sink_ctx.sink_channel);
            if (!db) {
                last_error_ = "database sink cast failed";
                return -1;
            }
            if (sink_ctx.table_name.empty()) {
                last_error_ = "builtin stream operator requires explicit table, use INTO <db_type>.<db_name>.<table>";
                return -1;
            }
            output_ = StreamChannelAdapter::MakeDatabaseWriter(
                "stream_adapter",
                sink_ctx.into_raw,
                std::shared_ptr<IDatabaseChannel>(db, [](IDatabaseChannel*) {}),
                sink_ctx.table_name);
            return output_ ? 0 : -1;
        }
        last_error_ = "unsupported sink type";
        return -1;
    }

    int OnSchemaReady(std::shared_ptr<arrow::Schema>) override { return 0; }

    int Process(const arrow::RecordBatch& batch, int64_t ts_ms) override {
        if (!output_) return -1;
        return output_->Put(batch.Slice(0, batch.num_rows()), ts_ms);
    }

    int Tick(int64_t) override { return 0; }
    int Flush() override { return 0; }
    std::string GetStats() override { return "{}"; }
    std::string LastError() override { return last_error_; }

    ParallelStrategy GetParallelStrategy() const override {
        return ParallelStrategy::STATELESS;
    }
    int GetParallelism() const override {
        return 4;
    }

 private:
    std::shared_ptr<IStreamChannel> output_;
    std::string last_error_;
};

class DbDirectWriterStreamOperator final : public IOperator, public IStreamOperator {
 public:
    std::string Category() override { return "custom"; }
    std::string Name() override { return "db_direct_writer_stream"; }
    std::string Description() override { return "test stream op writes to database channel directly"; }
    OperatorPosition Position() override { return OperatorPosition::DATA; }

    int Work(IChannel*, IChannel*) override { return -1; }
    int Configure(const char*, const char*) override { return 0; }

    int Init(const char*, const StreamSinkContext& sink_ctx) override {
        last_error_.clear();
        auto* db = dynamic_cast<IDatabaseChannel*>(sink_ctx.sink_channel);
        if (!db) {
            last_error_ = "output must be IDatabaseChannel";
            return -1;
        }
        db_ = std::shared_ptr<IDatabaseChannel>(db, [](IDatabaseChannel*) {});
        total_rows_ = 0;
        return 0;
    }

    int OnSchemaReady(std::shared_ptr<arrow::Schema>) override {
        if (!db_) return -1;
        if (db_->ExecuteSql("CREATE TABLE IF NOT EXISTS t44_two_segment_custom(total_rows INTEGER)") != 0) {
            last_error_ = "create table failed";
            return -1;
        }
        return 0;
    }

    int Process(const arrow::RecordBatch& batch, int64_t) override {
        total_rows_ += batch.num_rows();
        return 0;
    }

    int Tick(int64_t) override { return 0; }

    int Flush() override {
        if (!db_) return 0;
        if (total_rows_ <= 0) return 0;
        const std::string sql =
            "INSERT INTO t44_two_segment_custom(total_rows) VALUES (" + std::to_string(total_rows_) + ")";
        if (db_->ExecuteSql(sql.c_str()) != 0) {
            last_error_ = "insert failed";
            return -1;
        }
        return 0;
    }

    std::string GetStats() override { return "{}"; }
    std::string LastError() override { return last_error_; }

 private:
    std::shared_ptr<IDatabaseChannel> db_;
    int64_t total_rows_ = 0;
    std::string last_error_;
};

static bool ListContainsTaskId(const std::string& rsp, const std::string& task_id) {
    rapidjson::Document d;
    d.Parse(rsp.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("tasks") || !d["tasks"].IsArray()) {
        return false;
    }
    const auto& arr = d["tasks"];
    for (rapidjson::SizeType i = 0; i < arr.Size(); ++i) {
        const auto& item = arr[i];
        if (!item.IsObject()) continue;
        if (!item.HasMember("task_id") || !item["task_id"].IsString()) continue;
        if (task_id == item["task_id"].GetString()) return true;
    }
    return false;
}

static fnRouterHandler FindRouteHandler(PluginLoader* loader, const char* method, const char* uri) {
    fnRouterHandler h;
    loader->Traverse(IID_ROUTER_HANDLE, [&](void* p) -> int {
        auto* rh = static_cast<IRouterHandle*>(p);
        rh->EnumRoutes([&](const RouteItem& item) {
            if (item.method == method && item.uri == uri) {
                h = item.handler;
            }
        });
        return h ? -1 : 0;
    });
    return h;
}

static fnRouterHandler FindExecuteHandler(PluginLoader* loader) {
    return FindRouteHandler(loader, "POST", "/scheduler/batch/execute");
}

static void SeedSourceTable(IDatabaseChannel* db, const char* table) {
    auto schema = arrow::schema({
        arrow::field("id", arrow::int64()),
        arrow::field("name", arrow::utf8()),
        arrow::field("score", arrow::float64()),
    });
    arrow::Int64Builder id_b;
    arrow::StringBuilder name_b;
    arrow::DoubleBuilder score_b;
    (void)id_b.Append(1); (void)id_b.Append(2); (void)id_b.Append(3);
    (void)name_b.Append("a"); (void)name_b.Append("b"); (void)name_b.Append("c");
    (void)score_b.Append(10.0); (void)score_b.Append(20.0); (void)score_b.Append(30.0);
    auto batch = arrow::RecordBatch::Make(schema, 3, {
        id_b.Finish().ValueOrDie(),
        name_b.Finish().ValueOrDie(),
        score_b.Finish().ValueOrDie(),
    });

    IBatchWriter* writer = nullptr;
    ASSERT_EQ(db->CreateWriter(table, &writer), 0);
    ASSERT_TRUE(writer != nullptr);
    auto buf = SerializeBatch(batch);
    ASSERT_EQ(writer->Write(buf->data(), static_cast<size_t>(buf->size())), 0);
    BatchWriteStats stats;
    writer->Close(&stats);
    writer->Release();
    ASSERT_EQ(stats.rows_written, 3);
}

static int64_t QueryCount(IDatabaseChannel* db, const std::string& query) {
    IBatchReader* reader = nullptr;
    if (db->CreateReader(query.c_str(), &reader) != 0 || !reader) return -1;
    int64_t rows = 0;
    while (true) {
        const uint8_t* data = nullptr;
        size_t len = 0;
        int rc = reader->Next(&data, &len);
        if (rc == 1) break;
        if (rc != 0) {
            rows = -1;
            break;
        }
        rows += CountRowsInIpc(data, len);
    }
    reader->Close();
    reader->Release();
    return rows;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("=== Scheduler E2E Tests (Story 9.3) ===");

    const std::string suffix = std::to_string(::getpid());
    const std::filesystem::path db_path = std::filesystem::temp_directory_path() / ("flowsql_s9_3_" + suffix + ".db");
    const std::filesystem::path data_dir = std::filesystem::temp_directory_path() / ("flowsql_s9_3_df_" + suffix);
    const std::filesystem::path operator_db_dir = std::filesystem::temp_directory_path() / ("flowsql_s9_3_catalog_" + suffix);
    const std::filesystem::path stream_cfg = std::filesystem::temp_directory_path() / ("flowsql_s9_3_stream_" + suffix + ".yml");
    const std::filesystem::path stream_meta_db = std::filesystem::temp_directory_path() / ("flowsql_s9_3_stream_meta_" + suffix + ".db");
    std::filesystem::remove(db_path);
    std::filesystem::remove(stream_cfg);
    std::filesystem::remove(stream_meta_db);
    std::filesystem::create_directories(data_dir);
    std::filesystem::create_directories(operator_db_dir);

    {
        std::ofstream out(stream_cfg);
        ASSERT_TRUE(out.is_open());
        out << "channels:\n";
        out << "  stream_channels:\n";
        out << "    - type: ring\n";
        out << "      name: in\n";
        out << "      option: \"ring_size=256;batch_rows=128;overflow=drop;ring_mode=spsc;finite=false\"\n";
        out << "    - type: ring\n";
        out << "      name: out\n";
        out << "      option: \"ring_size=256;batch_rows=128;overflow=drop;ring_mode=spsc;finite=false\"\n";
        out << "    - type: ring\n";
        out << "      name: stop_in\n";
        out << "      option: \"ring_size=256;batch_rows=128;overflow=drop;ring_mode=spsc;finite=false\"\n";
        out << "    - type: ring\n";
        out << "      name: stop_out\n";
        out << "      option: \"ring_size=256;batch_rows=128;overflow=drop;ring_mode=spsc;finite=false\"\n";
        out << "    - type: ring\n";
        out << "      name: svc_out\n";
        out << "      option: \"ring_size=256;batch_rows=128;overflow=drop;ring_mode=spsc;finite=false\"\n";
        out << "    - type: tcp_session_mock\n";
        out << "      name: tcp_src\n";
        out << "      option: \"mode=keyed;total_records=64;batch_rows=8;partition_count=4;emit_interval_ms=0;ring_size=256;overflow=drop\"\n";
        out << "    - type: tcp_session_mock\n";
        out << "      name: tcp_src_stateless\n";
        out << "      option: \"mode=stateless;total_records=64;batch_rows=8;emit_interval_ms=0;ring_size=256;overflow=drop\"\n";
        out.flush();
    }

    PluginLoader* loader = PluginLoader::Single();
    const char* libs[] = {"libflowsql_database.so", "libflowsql_builtin.so", "libflowsql_catalog.so", "libflowsql_scheduler.so", "libflowsql_stream.so"};
    std::string db_opt = "type=sqlite;name=local;path=" + db_path.string();
    std::string catalog_opt = "data_dir=" + data_dir.string() + ";operator_db_dir=" + operator_db_dir.string();
    std::string stream_opt = "config_file=" + stream_cfg.string() + ";db_path=" + stream_meta_db.string();
    const char* opts[] = {db_opt.c_str(), nullptr, catalog_opt.c_str(), nullptr, stream_opt.c_str()};
    ASSERT_EQ(loader->Load(get_absolute_process_path(), libs, opts, 5), 0);
    std::puts("[INFO] plugins loaded");
    ASSERT_EQ(loader->StartAll(), 0);
    std::puts("[INFO] plugins started");

    auto* factory = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    auto* registry = static_cast<IChannelRegistry*>(loader->First(IID_CHANNEL_REGISTRY));
    auto* stream_factory = static_cast<IStreamFactory*>(loader->First(IID_STREAM_FACTORY));
    auto* op_registry = static_cast<IOperatorRegistry*>(loader->First(IID_OPERATOR_REGISTRY));
    ASSERT_TRUE(factory != nullptr);
    ASSERT_TRUE(registry != nullptr);
    ASSERT_TRUE(stream_factory != nullptr);
    ASSERT_TRUE(op_registry != nullptr);
    auto* db = dynamic_cast<IDatabaseChannel*>(factory->Get("sqlite", "local"));
    ASSERT_TRUE(db != nullptr);

    SeedSourceTable(db, "src");
    std::puts("[INFO] source table seeded");
    auto exec = FindExecuteHandler(loader);
    auto stream_exec = FindRouteHandler(loader, "POST", "/scheduler/stream/execute");
    auto stream_stop = FindRouteHandler(loader, "POST", "/scheduler/stream/stop");
    auto stream_status = FindRouteHandler(loader, "POST", "/scheduler/stream/status");
    auto stream_list = FindRouteHandler(loader, "POST", "/scheduler/stream/list");
    auto stream_add = FindRouteHandler(loader, "POST", "/channels/stream/add");
    auto stream_definitions_query = FindRouteHandler(loader, "POST", "/channels/stream/definitions/query");
    auto sql_classify = FindRouteHandler(loader, "POST", "/scheduler/sql/classify");
    auto activate = FindRouteHandler(loader, "POST", "/operators/activate");
    auto deactivate = FindRouteHandler(loader, "POST", "/operators/deactivate");
    auto upsert_batch = FindRouteHandler(loader, "POST", "/operators/upsert_batch");
    ASSERT_TRUE(exec != nullptr);
    ASSERT_TRUE(stream_exec != nullptr);
    ASSERT_TRUE(stream_stop != nullptr);
    ASSERT_TRUE(stream_status != nullptr);
    ASSERT_TRUE(stream_list != nullptr);
    ASSERT_TRUE(stream_add != nullptr);
    ASSERT_TRUE(stream_definitions_query != nullptr);
    ASSERT_TRUE(sql_classify != nullptr);
    ASSERT_TRUE(activate != nullptr);
    ASSERT_TRUE(deactivate != nullptr);
    ASSERT_TRUE(upsert_batch != nullptr);
    std::puts("[INFO] execute handler ready");

    // T17a: SQL classify 返回批/流类型
    {
        std::string rsp;
        ASSERT_EQ(sql_classify("/scheduler/sql/classify",
                               MakeReq("SELECT * FROM sqlite.local.src INTO dataframe.classify_batch"),
                               rsp),
                  error::OK);
        rapidjson::Document batch_doc;
        batch_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!batch_doc.HasParseError() && batch_doc.IsObject());
        ASSERT_TRUE(batch_doc.HasMember("task_kind") && batch_doc["task_kind"].IsString());
        ASSERT_EQ(std::string(batch_doc["task_kind"].GetString()), "batch");

        ASSERT_EQ(sql_classify("/scheduler/sql/classify",
                               MakeReq("SELECT * FROM tcp_session_mock.tcp_src USING builtin.tcp_service_merge_stream INTO dataframe.classify_stream"),
                               rsp),
                  error::OK);
        rapidjson::Document stream_doc;
        stream_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!stream_doc.HasParseError() && stream_doc.IsObject());
        ASSERT_TRUE(stream_doc.HasMember("task_kind") && stream_doc["task_kind"].IsString());
        ASSERT_EQ(std::string(stream_doc["task_kind"].GetString()), "stream");
    }
    std::puts("[PASS] T17a");

    // T17b: Stream 通道定义元数据查询
    {
        std::string rsp;
        ASSERT_EQ(stream_definitions_query("/channels/stream/definitions/query", "{}", rsp), error::OK);
        rapidjson::Document doc;
        doc.Parse(rsp.c_str());
        ASSERT_TRUE(!doc.HasParseError() && doc.IsObject());
        ASSERT_TRUE(doc.HasMember("definitions") && doc["definitions"].IsArray());

        bool found_ring = false;
        bool found_stream_hub = false;
        for (const auto& item : doc["definitions"].GetArray()) {
            ASSERT_TRUE(item.IsObject());
            ASSERT_TRUE(item.HasMember("channel_type") && item["channel_type"].IsString());
            ASSERT_TRUE(item.HasMember("display_name") && item["display_name"].IsString());
            ASSERT_TRUE(item.HasMember("allowed_roles") && item["allowed_roles"].IsArray());
            ASSERT_TRUE(item.HasMember("option_schema") && item["option_schema"].IsArray());
            const std::string channel_type = item["channel_type"].GetString();
            if (channel_type == "ring") found_ring = true;
            if (channel_type == "stream_hub") found_stream_hub = true;
        }
        ASSERT_TRUE(found_ring);
        ASSERT_TRUE(found_stream_hub);
    }
    std::puts("[PASS] T17b");

    // T18: INTO dataframe.result 后可通过 Registry 读取
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM sqlite.local.src INTO dataframe.result"), rsp);
        ASSERT_EQ(rc, error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("result"));
        ASSERT_TRUE(ch != nullptr);
        ASSERT_EQ(QueryCount(db, "SELECT * FROM src"), 3);
    }
    std::puts("[PASS] T18");

    // T19: FROM dataframe.result INTO sqlite.local.t2
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.result INTO sqlite.local.t2"), rsp);
        ASSERT_EQ(rc, error::OK);
        ASSERT_EQ(QueryCount(db, "SELECT * FROM t2"), 3);
    }
    std::puts("[PASS] T19");

    // T20: FROM dataframe.<不存在> 返回 NOT_FOUND
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.not_exists INTO sqlite.local.t3"), rsp);
        ASSERT_EQ(rc, error::NOT_FOUND);
    }
    std::puts("[PASS] T20");

    // T21: INTO dataframe.result 覆盖语义（第二次覆盖第一次）
    {
        std::string rsp;
        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM sqlite.local.src WHERE id <= 2 INTO dataframe.result"), rsp),
                  error::OK);
        auto ch1 = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("result"));
        ASSERT_TRUE(ch1 != nullptr);

        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM sqlite.local.src WHERE id > 2 INTO dataframe.result"), rsp),
                  error::OK);
        auto ch2 = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("result"));
        ASSERT_TRUE(ch2 != nullptr);

        DataFrame out;
        ASSERT_EQ(ch2->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 1);
    }
    std::puts("[PASS] T21");

    // T22: 无 INTO 的匿名结果行为不变（仅响应返回，不落入 Registry 新名称）
    {
        std::string rsp;
        size_t before = 0;
        registry->List([&](const char*, std::shared_ptr<IChannel>) { ++before; });
        ASSERT_EQ(exec("/scheduler/batch/execute", MakeReq("SELECT * FROM sqlite.local.src"), rsp), error::OK);
        rapidjson::Document doc;
        doc.Parse(rsp.c_str());
        ASSERT_TRUE(!doc.HasParseError() && doc.IsObject());
        ASSERT_TRUE(doc.HasMember("rows"));
        ASSERT_TRUE(doc.HasMember("result_row_count"));
        ASSERT_EQ(doc["rows"].GetInt64(), 3);
        ASSERT_EQ(doc["result_row_count"].GetInt64(), 3);
        size_t after = 0;
        registry->List([&](const char*, std::shared_ptr<IChannel>) { ++after; });
        ASSERT_EQ(after, before);
    }
    std::puts("[PASS] T22");

    // T23: 跨通道链路 + 内置算子 passthrough
    {
        std::string activate_rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough"})", activate_rsp), error::OK);

        std::string rsp;
        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM sqlite.local.src USING builtin.passthrough INTO dataframe.out"), rsp),
                  error::OK);
        ASSERT_TRUE(std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("out")) != nullptr);
        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM dataframe.out INTO sqlite.local.t_passthrough"), rsp),
                  error::OK);
        ASSERT_EQ(QueryCount(db, "SELECT * FROM t_passthrough"), 3);
    }
    std::puts("[PASS] T23");

    // T24: 去激活仅阻止新任务，不中断已 running 任务
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough"})", rsp), error::OK);

        int32_t running_rc = error::INTERNAL_ERROR;
        std::thread worker([&]() {
            std::string local_rsp;
            running_rc = exec("/scheduler/batch/execute",
                              MakeReq("SELECT * FROM sqlite.local.src USING builtin.passthrough "
                                      "WITH delay_ms=800 INTO dataframe.running_out"),
                              local_rsp);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        ASSERT_EQ(deactivate("/operators/deactivate", R"({"name":"builtin.passthrough"})", rsp), error::OK);

        worker.join();
        ASSERT_EQ(running_rc, error::OK);
        ASSERT_TRUE(std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("running_out")) != nullptr);

        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM sqlite.local.src USING builtin.passthrough INTO dataframe.blocked"), rsp),
                  error::CONFLICT);
    }
    std::puts("[PASS] T24");

    // T25: 双算子串行链路成功（每个算子独立 WITH）
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough"})", rsp), error::OK);
        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM sqlite.local.src "
                               "USING builtin.passthrough WITH delay_ms=10 "
                               "THEN builtin.passthrough WITH delay_ms=0 "
                               "INTO dataframe.chain_ok"),
                       rsp),
                  error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("chain_ok"));
        ASSERT_TRUE(ch != nullptr);
        DataFrame out;
        ASSERT_EQ(ch->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 3);
    }
    std::puts("[PASS] T25");

    // T26: 双算子链第 2 步失败（独立 WITH 不复用）
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough"})", rsp), error::OK);
        size_t before_cnt = 0;
        registry->List([&](const char*, std::shared_ptr<IChannel>) { ++before_cnt; });
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM sqlite.local.src "
                                  "USING builtin.passthrough WITH force_fail=0 "
                                  "THEN builtin.passthrough WITH force_fail=1 "
                                  "INTO dataframe.chain_fail"),
                          rsp);
        ASSERT_EQ(rc, error::INTERNAL_ERROR);
        ASSERT_TRUE(std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("chain_fail")) == nullptr);

        size_t after_cnt = 0;
        registry->List([&](const char*, std::shared_ptr<IChannel>) { ++after_cnt; });
        ASSERT_EQ(after_cnt, before_cnt);  // 失败后不应新增/泄漏具名通道

        // 失败后再次执行，验证执行器状态未污染
        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT * FROM sqlite.local.src "
                               "USING builtin.passthrough WITH force_fail=0 "
                               "THEN builtin.passthrough WITH force_fail=0 "
                               "INTO dataframe.chain_after_fail"),
                       rsp),
                  error::OK);
        ASSERT_TRUE(std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("chain_after_fail")) != nullptr);
    }
    std::puts("[PASS] T26");

    // T27: 多源 + USING builtin.passthrough 走统一多输入入口（默认回退到首输入）
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.result,dataframe.out "
                                  "USING builtin.passthrough INTO dataframe.multi_ok"),
                          rsp);
        ASSERT_EQ(rc, error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("multi_ok"));
        ASSERT_TRUE(ch != nullptr);

        DataFrame out;
        ASSERT_EQ(ch->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 1);  // dataframe.result 在 T21 被覆盖为 1 行
        ASSERT_EQ(std::get<int64_t>(out.GetRow(0)[0]), 3);
    }
    std::puts("[PASS] T27");

    // T28: 多源无 USING 算子应报 BAD_REQUEST
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.result,dataframe.out INTO dataframe.multi_no_op"),
                          rsp);
        ASSERT_EQ(rc, error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("multi-source FROM requires USING operator") != std::string::npos);
    }
    std::puts("[PASS] T28");

    // T29: 多源包含非 dataframe.* 应报 BAD_REQUEST
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM sqlite.local.src,dataframe.result "
                                  "USING builtin.passthrough INTO dataframe.multi_mixed"),
                          rsp);
        ASSERT_EQ(rc, error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("multi-source FROM only supports dataframe.* in Sprint 10") != std::string::npos);
    }
    std::puts("[PASS] T29");

    // T30: 多源 + WHERE（Sprint10 V1）应报 BAD_REQUEST
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.result,dataframe.out "
                                  "WHERE id > 1 USING builtin.passthrough INTO dataframe.multi_where"),
                          rsp);
        ASSERT_EQ(rc, error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("multi-source FROM does not support WHERE in Sprint 10") != std::string::npos);
    }
    std::puts("[PASS] T30");

    // T31: INTO 非法目标（未限定名）应报 BAD_REQUEST
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM sqlite.local.src INTO t2"),
                          rsp);
        ASSERT_EQ(rc, error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("invalid INTO destination") != std::string::npos);
    }
    std::puts("[PASS] T31");

    // T32: 激活 concat/hstack
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.concat"})", rsp), error::OK);
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.hstack"})", rsp), error::OK);
    }
    std::puts("[PASS] T32");

    // T33: concat 成功（按行合并）
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.out,dataframe.chain_ok "
                                  "USING builtin.concat INTO dataframe.concat_ok"),
                          rsp);
        ASSERT_EQ(rc, error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("concat_ok"));
        ASSERT_TRUE(ch != nullptr);

        DataFrame out;
        ASSERT_EQ(ch->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 6);
        ASSERT_EQ(out.GetSchema().size(), 3u);
    }
    std::puts("[PASS] T33");

    // T34: concat schema 不兼容应失败
    {
        std::string rsp;
        ASSERT_EQ(exec("/scheduler/batch/execute",
                       MakeReq("SELECT id FROM sqlite.local.src INTO dataframe.only_id"),
                       rsp),
                  error::OK);

        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.out,dataframe.only_id "
                                  "USING builtin.concat INTO dataframe.concat_bad"),
                          rsp);
        ASSERT_EQ(rc, error::INTERNAL_ERROR);
        ASSERT_TRUE(rsp.find("concat schema mismatch") != std::string::npos);
    }
    std::puts("[PASS] T34");

    // T35: hstack 成功（按列合并）
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.out,dataframe.chain_ok "
                                  "USING builtin.hstack INTO dataframe.hstack_ok"),
                          rsp);
        ASSERT_EQ(rc, error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("hstack_ok"));
        ASSERT_TRUE(ch != nullptr);

        DataFrame out;
        ASSERT_EQ(ch->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 3);
        ASSERT_EQ(out.GetSchema().size(), 6u);
    }
    std::puts("[PASS] T35");

    // T36: hstack 行数不一致应失败
    {
        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.result,dataframe.out "
                                  "USING builtin.hstack INTO dataframe.hstack_bad"),
                          rsp);
        ASSERT_EQ(rc, error::INTERNAL_ERROR);
        ASSERT_TRUE(rsp.find("hstack row count mismatch") != std::string::npos);
    }
    std::puts("[PASS] T36");

    // T37: concat 覆盖多类型（INT32/INT64/FLOAT/DOUBLE/STRING/BOOL）
    {
        auto build_typed_channel = [&](const char* name, int32_t base) {
            auto ch = std::make_shared<DataFrameChannel>("dataframe", name);
            ch->Open();

            DataFrame df;
            df.SetSchema({
                {"c_i32", DataType::INT32, 0, ""},
                {"c_i64", DataType::INT64, 0, ""},
                {"c_f32", DataType::FLOAT, 0, ""},
                {"c_f64", DataType::DOUBLE, 0, ""},
                {"c_str", DataType::STRING, 0, ""},
                {"c_bool", DataType::BOOLEAN, 0, ""},
            });
            df.AppendRow({base + 1, int64_t(base + 1000), float(base + 0.5f), double(base + 0.25), std::string("n") + std::to_string(base + 1), true});
            df.AppendRow({base + 2, int64_t(base + 2000), float(base + 1.5f), double(base + 1.25), std::string("n") + std::to_string(base + 2), false});
            ASSERT_EQ(ch->Write(&df), 0);

            (void)registry->Unregister(name);
            ASSERT_EQ(registry->Register(name, std::static_pointer_cast<IChannel>(ch)), 0);
        };

        build_typed_channel("typed_a", 10);
        build_typed_channel("typed_b", 20);

        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.typed_a,dataframe.typed_b "
                                  "USING builtin.concat INTO dataframe.concat_types"),
                          rsp);
        ASSERT_EQ(rc, error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("concat_types"));
        ASSERT_TRUE(ch != nullptr);

        DataFrame out;
        ASSERT_EQ(ch->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 4);
        auto schema = out.GetSchema();
        ASSERT_EQ(schema.size(), size_t(6));
        ASSERT_EQ(schema[0].type, DataType::INT32);
        ASSERT_EQ(schema[1].type, DataType::INT64);
        ASSERT_EQ(schema[2].type, DataType::FLOAT);
        ASSERT_EQ(schema[3].type, DataType::DOUBLE);
        ASSERT_EQ(schema[4].type, DataType::STRING);
        ASSERT_EQ(schema[5].type, DataType::BOOLEAN);

        auto row3 = out.GetRow(3);
        ASSERT_EQ(std::get<int32_t>(row3[0]), 22);
        ASSERT_EQ(std::get<int64_t>(row3[1]), 2020);
        ASSERT_EQ(std::get<std::string>(row3[4]), "n22");
        ASSERT_EQ(std::get<bool>(row3[5]), false);
    }
    std::puts("[PASS] T37");

    // T38: hstack 覆盖多类型（按列合并）
    {
        auto left = std::make_shared<DataFrameChannel>("dataframe", "hleft");
        auto right = std::make_shared<DataFrameChannel>("dataframe", "hright");
        left->Open();
        right->Open();

        DataFrame ldf;
        ldf.SetSchema({
            {"a_i32", DataType::INT32, 0, ""},
            {"a_str", DataType::STRING, 0, ""},
            {"a_bool", DataType::BOOLEAN, 0, ""},
        });
        ldf.AppendRow({int32_t(1), std::string("x"), true});
        ldf.AppendRow({int32_t(2), std::string("y"), false});
        ASSERT_EQ(left->Write(&ldf), 0);

        DataFrame rdf;
        rdf.SetSchema({
            {"b_i64", DataType::INT64, 0, ""},
            {"b_f32", DataType::FLOAT, 0, ""},
            {"b_f64", DataType::DOUBLE, 0, ""},
        });
        rdf.AppendRow({int64_t(100), float(1.5f), double(10.25)});
        rdf.AppendRow({int64_t(200), float(2.5f), double(20.25)});
        ASSERT_EQ(right->Write(&rdf), 0);

        (void)registry->Unregister("hleft");
        (void)registry->Unregister("hright");
        ASSERT_EQ(registry->Register("hleft", std::static_pointer_cast<IChannel>(left)), 0);
        ASSERT_EQ(registry->Register("hright", std::static_pointer_cast<IChannel>(right)), 0);

        std::string rsp;
        int32_t rc = exec("/scheduler/batch/execute",
                          MakeReq("SELECT * FROM dataframe.hleft,dataframe.hright "
                                  "USING builtin.hstack INTO dataframe.hstack_types"),
                          rsp);
        ASSERT_EQ(rc, error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("hstack_types"));
        ASSERT_TRUE(ch != nullptr);

        DataFrame out;
        ASSERT_EQ(ch->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 2);
        auto schema = out.GetSchema();
        ASSERT_EQ(schema.size(), size_t(6));
        ASSERT_EQ(schema[0].type, DataType::INT32);
        ASSERT_EQ(schema[1].type, DataType::STRING);
        ASSERT_EQ(schema[2].type, DataType::BOOLEAN);
        ASSERT_EQ(schema[3].type, DataType::INT64);
        ASSERT_EQ(schema[4].type, DataType::FLOAT);
        ASSERT_EQ(schema[5].type, DataType::DOUBLE);

        auto row0 = out.GetRow(0);
        ASSERT_EQ(std::get<int32_t>(row0[0]), 1);
        ASSERT_EQ(std::get<std::string>(row0[1]), "x");
        ASSERT_EQ(std::get<bool>(row0[2]), true);
        ASSERT_EQ(std::get<int64_t>(row0[3]), 100);
    }
    std::puts("[PASS] T38");

    // T39: 流式 execute/status/list + 数据流转
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough_stream"})", rsp), error::OK);

        auto* stream_in = stream_factory->Get("ring", "in");
        auto* stream_out = stream_factory->Get("ring", "out");
        ASSERT_TRUE(stream_in != nullptr);
        ASSERT_TRUE(stream_out != nullptr);

        // 清理输出通道残留数据
        while (true) {
            PollEvent ev = stream_out->PollNext(0);
            if (ev.kind != PollEventKind::kData) break;
        }

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeReq("SELECT * FROM ring.in USING builtin.passthrough_stream INTO stream.out"),
                              rsp),
                  error::OK);
        const std::string task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!task_id.empty());

        ASSERT_EQ(stream_in->Put(MakeStreamBatch(1), 1001), 0);
        ASSERT_EQ(stream_in->Put(MakeStreamBatch(2), 1002), 0);
        ASSERT_EQ(stream_in->Put(MakeStreamBatch(3), 1003), 0);
        stream_in->CloseStream();

        std::string final_status;
        bool done = false;
        for (int i = 0; i < 300; ++i) {
            std::string s_rsp;
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(task_id), s_rsp), error::OK);
            final_status = ParseStatus(s_rsp);
            if (final_status == "stopped" || final_status == "cancelled" || final_status == "failed") {
                done = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(done);
        ASSERT_EQ(final_status, "stopped");

        int rows = 0;
        for (int i = 0; i < 200 && rows < 3; ++i) {
            PollEvent ev = stream_out->PollNext(10);
            if (ev.kind == PollEventKind::kData && ev.batch.data) {
                rows += static_cast<int>(ev.batch.data->num_rows());
            }
        }
        ASSERT_EQ(rows, 3);

        std::string list_rsp;
        ASSERT_EQ(stream_list("/scheduler/stream/list", "{}", list_rsp), error::OK);
        ASSERT_TRUE(ListContainsTaskId(list_rsp, task_id));
    }
    std::puts("[PASS] T39");

    // T40: 流式 stop 终止运行中任务
    {
        std::string rsp;
        auto* stop_in = stream_factory->Get("ring", "stop_in");
        auto* stop_out = stream_factory->Get("ring", "stop_out");
        ASSERT_TRUE(stop_in != nullptr);
        ASSERT_TRUE(stop_out != nullptr);

        while (true) {
            PollEvent ev = stop_out->PollNext(0);
            if (ev.kind != PollEventKind::kData) break;
        }

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeReq("SELECT * FROM ring.stop_in USING builtin.passthrough_stream INTO stream.stop_out"),
                              rsp),
                  error::OK);
        const std::string task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!task_id.empty());

        bool seen_running = false;
        for (int i = 0; i < 100; ++i) {
            std::string s_rsp;
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(task_id), s_rsp), error::OK);
            const std::string st = ParseStatus(s_rsp);
            if (st == "running" || st == "stopping") {
                seen_running = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(seen_running);

        std::string stop_rsp;
        ASSERT_EQ(stream_stop("/scheduler/stream/stop", MakeTaskReq(task_id), stop_rsp), error::OK);
        const std::string stop_status = ParseStatus(stop_rsp);
        ASSERT_EQ(stop_status, "stopped");

        std::string status_rsp;
        ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(task_id), status_rsp), error::OK);
        const std::string final_status = ParseStatus(status_rsp);
        ASSERT_EQ(final_status, "stopped");

        std::string list_rsp;
        ASSERT_EQ(stream_list("/scheduler/stream/list", "{}", list_rsp), error::OK);
        ASSERT_TRUE(ListContainsTaskId(list_rsp, task_id));
    }
    std::puts("[PASS] T40");

    // T41: 内置 tcp_session_mock 通道 + tcp_service_merge_stream 算子链路
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.tcp_service_merge_stream"})", rsp), error::OK);

        auto* tcp_src = stream_factory->Get("tcp_session_mock", "tcp_src");
        auto* svc_out = stream_factory->Get("ring", "svc_out");
        ASSERT_TRUE(tcp_src != nullptr);
        ASSERT_TRUE(svc_out != nullptr);

        while (true) {
            PollEvent ev = svc_out->PollNext(0);
            if (ev.kind != PollEventKind::kData) break;
        }

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeReq("SELECT * FROM tcp_session_mock.tcp_src "
                                      "USING builtin.tcp_service_merge_stream INTO stream.svc_out"),
                              rsp),
                  error::OK);
        const std::string task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!task_id.empty());

        std::string final_status;
        bool done = false;
        for (int i = 0; i < 300; ++i) {
            std::string s_rsp;
            ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(task_id), s_rsp), error::OK);
            final_status = ParseStatus(s_rsp);
            if (final_status == "stopped" || final_status == "cancelled" || final_status == "failed") {
                done = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(done);
        ASSERT_EQ(final_status, "stopped");

        int out_rows = 0;
        bool schema_ok = false;
        for (int i = 0; i < 200; ++i) {
            PollEvent ev = svc_out->PollNext(20);
            if (ev.kind != PollEventKind::kData || !ev.batch.data) continue;
            out_rows += static_cast<int>(ev.batch.data->num_rows());
            auto schema = ev.batch.data->schema();
            ASSERT_TRUE(schema != nullptr);
            ASSERT_TRUE(schema->GetFieldIndex("clientIP") >= 0);
            ASSERT_TRUE(schema->GetFieldIndex("serverIP") >= 0);
            ASSERT_TRUE(schema->GetFieldIndex("serverPort") >= 0);
            ASSERT_TRUE(schema->GetFieldIndex("bps") >= 0);
            ASSERT_TRUE(schema->GetFieldIndex("pps") >= 0);
            ASSERT_TRUE(schema->GetFieldIndex("clientPort") < 0);
            schema_ok = true;
            if (out_rows > 0) break;
        }
        ASSERT_TRUE(schema_ok);
        ASSERT_TRUE(out_rows > 0);
    }
    std::puts("[PASS] T41");

    // T42: 非 stream sink 在并行写能力不足时直接失败（无隐式降级）
    {
        std::string rsp;
        ASSERT_EQ(op_registry->Register("custom.parallel_passthrough_stream", []() -> IOperator* {
            return new ParallelPassthroughStreamOperator();
        }), 0);
        ASSERT_EQ(upsert_batch("/operators/upsert_batch", R"({
            "operators":[
                {
                    "category":"custom",
                    "name":"parallel_passthrough_stream",
                    "type":"cpp",
                    "source":"e2e",
                    "description":"e2e parallel passthrough stream",
                    "position":"DATA"
                }
            ]
        })", rsp), error::OK);
        ASSERT_EQ(activate("/operators/activate", R"({"name":"custom.parallel_passthrough_stream"})", rsp), error::OK);

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeReq("SELECT * FROM tcp_session_mock.tcp_src_stateless "
                                      "USING custom.parallel_passthrough_stream INTO dataframe.stream_single_writer"),
                              rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_SINK_CAPABILITY_MISMATCH") != std::string::npos);
    }
    std::puts("[PASS] T42");

    // T43: INTO 数据库目标规则（builtin 两段式报错；普通算子可接收两段式 DB 通道）
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough_stream"})", rsp), error::OK);
        ASSERT_EQ(op_registry->Register("custom.db_direct_writer_stream", []() -> IOperator* {
            return new DbDirectWriterStreamOperator();
        }), 0);
        ASSERT_EQ(upsert_batch("/operators/upsert_batch", R"({
            "operators":[
                {
                    "category":"custom",
                    "name":"db_direct_writer_stream",
                    "type":"cpp",
                    "source":"e2e",
                    "description":"e2e custom stream db writer",
                    "position":"DATA"
                }
            ]
        })", rsp), error::OK);
        ASSERT_EQ(activate("/operators/activate", R"({"name":"custom.db_direct_writer_stream"})", rsp), error::OK);

        auto wait_stream_terminal = [&](const std::string& task_id, std::string* final_status) -> bool {
            for (int i = 0; i < 400; ++i) {
                std::string s_rsp;
                if (stream_status("/scheduler/stream/status", MakeTaskReq(task_id), s_rsp) != error::OK) {
                    return false;
                }
                const std::string st = ParseStatus(s_rsp);
                if (st == "stopped" || st == "cancelled" || st == "failed") {
                    if (final_status) *final_status = st;
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return false;
        };

        // case 1: builtin + 三段式目标成功
        const int64_t src_rows_before = QueryCount(db, "SELECT * FROM src");
        ASSERT_EQ(src_rows_before, 3);
        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeReq("SELECT * FROM tcp_session_mock.tcp_src "
                                      "USING builtin.passthrough_stream "
                                      "INTO sqlite.local.t44_into"),
                              rsp),
                  error::OK);
        std::string task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!task_id.empty());
        std::string terminal_status;
        ASSERT_TRUE(wait_stream_terminal(task_id, &terminal_status));
        ASSERT_EQ(terminal_status, "stopped");
        ASSERT_TRUE(QueryCount(db, "SELECT * FROM t44_into") > 0);
        ASSERT_EQ(QueryCount(db, "SELECT * FROM src"), src_rows_before);

        // case 2: WITH sink_table 不再作为框架兜底语义
        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeReq("SELECT * FROM tcp_session_mock.tcp_src "
                                      "USING builtin.passthrough_stream WITH sink_table=t44_with "
                                      "INTO sqlite.local"),
                              rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("sink_table") != std::string::npos);

        // case 3: builtin + 两段式 DB 目标失败（要求显式三段式）
        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeReq("SELECT * FROM tcp_session_mock.tcp_src "
                                      "USING builtin.passthrough_stream INTO sqlite.local"),
                              rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("requires explicit table") != std::string::npos);

        // case 4: 普通算子可接收两段式 DB 通道并自行写入
        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeReq("SELECT * FROM tcp_session_mock.tcp_src "
                                      "USING custom.db_direct_writer_stream INTO sqlite.local"),
                              rsp),
                  error::OK);
        task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!task_id.empty());
        terminal_status.clear();
        ASSERT_TRUE(wait_stream_terminal(task_id, &terminal_status));
        ASSERT_EQ(terminal_status, "stopped");
        ASSERT_TRUE(QueryCount(db, "SELECT * FROM t44_two_segment_custom") > 0);
    }
    std::puts("[PASS] T43");

    // T44: Story 14.11 T5 回归（缺失注册/非法配置/重复创建）
    {
        std::string rsp;
        ASSERT_EQ(stream_add("/channels/stream/add",
                             R"({"type":"no_such_stream_type","name":"x_missing","option":""})",
                             rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("add stream channel failed") != std::string::npos ||
                    rsp.find("unsupported stream channel type") != std::string::npos);

        ASSERT_EQ(stream_add("/channels/stream/add",
                             R"({"type":"ring","name":"x_invalid","option":"ring_mode=spsc;ring_size=3;overflow=drop;finite=false"})",
                             rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("add stream channel failed") != std::string::npos ||
                    rsp.find("invalid option") != std::string::npos);

        ASSERT_EQ(stream_add("/channels/stream/add",
                             R"({"type":"ring","name":"x_dup","option":"ring_mode=spsc;ring_size=256;overflow=drop;finite=false;batch_rows=64"})",
                             rsp),
                  error::OK);
        ASSERT_EQ(stream_add("/channels/stream/add",
                             R"({"type":"ring","name":"x_dup","option":"ring_mode=spsc;ring_size=256;overflow=drop;finite=false;batch_rows=64"})",
                             rsp),
                  error::CONFLICT);
    }
    std::puts("[PASS] T44");

    // T46: Story 14.12 回归（结构化 add/query + split/merge selector 语义）
    {
        std::string rsp;
        ASSERT_EQ(stream_add("/channels/stream/add", R"({
            "type":"stream_hub",
            "name":"npm_hub",
            "role":"both",
            "options":{
                "mode":"split",
                "partition_count":2,
                "partition_ring_mode":"spsc",
                "partition_ring_size":256
            }
        })", rsp), error::OK);

        ASSERT_EQ(stream_add("/channels/stream/add", R"({
            "type":"stream_hub",
            "name":"npm_merge",
            "role":"both",
            "options":{
                "mode":"merge",
                "partition_count":2,
                "partition_ring_mode":"spsc",
                "partition_ring_size":256
            }
        })", rsp), error::OK);

        ASSERT_EQ(stream_add("/channels/stream/add", R"({
            "type":"ring",
            "name":"source_only_ring",
            "role":"source",
            "options":{
                "ring_mode":"spsc",
                "ring_size":256,
                "overflow":"drop",
                "finite":false
            }
        })", rsp), error::OK);

        ASSERT_EQ(stream_add("/channels/stream/add", R"({
            "type":"ring",
            "name":"sink_only_ring",
            "role":"sink",
            "options":{
                "ring_mode":"spsc",
                "ring_size":256,
                "overflow":"drop",
                "finite":false
            }
        })", rsp), error::OK);

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeReq("SELECT * FROM tcp_session_mock.tcp_src "
                                      "USING builtin.passthrough_stream INTO stream.source_only_ring"),
                              rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_CHANNEL_ROLE_MISMATCH") != std::string::npos);

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeReq("SELECT * FROM stream.sink_only_ring "
                                      "USING builtin.passthrough_stream INTO dataframe.sink_role_bad"),
                              rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_CHANNEL_ROLE_MISMATCH") != std::string::npos);

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeReq("SELECT * FROM tcp_session_mock.tcp_src "
                                      "USING builtin.passthrough_stream INTO stream.npm_hub[0]"),
                              rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_HUB_SELECTOR_NOT_ALLOWED_INTO") != std::string::npos);

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeReq("SELECT * FROM stream.npm_merge[*] "
                                      "USING builtin.passthrough_stream INTO dataframe.npm_merge_bad"),
                              rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_HUB_SELECTOR_NOT_ALLOWED_MERGE") != std::string::npos);

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeReq("SELECT * FROM tcp_session_mock.tcp_src "
                                      "USING builtin.passthrough_stream INTO stream.npm_hub"),
                              rsp),
                  error::OK);
        std::string producer_task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!producer_task_id.empty());

        auto wait_stream_terminal = [&](const std::string& task_id, std::string* final_status) -> bool {
            for (int i = 0; i < 500; ++i) {
                std::string s_rsp;
                if (stream_status("/scheduler/stream/status", MakeTaskReq(task_id), s_rsp) != error::OK) {
                    return false;
                }
                const std::string st = ParseStatus(s_rsp);
                if (st == "stopped" || st == "cancelled" || st == "failed") {
                    if (final_status) *final_status = st;
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return false;
        };

        std::string terminal_status;
        ASSERT_TRUE(wait_stream_terminal(producer_task_id, &terminal_status));
        ASSERT_EQ(terminal_status, "stopped");

        ASSERT_EQ(stream_exec("/scheduler/stream/execute",
                              MakeReq("SELECT * FROM stream.npm_hub "
                                      "USING builtin.passthrough_stream INTO dataframe.npm_auto"),
                              rsp),
                  error::OK);

        rapidjson::Document exec_doc;
        exec_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!exec_doc.HasParseError() && exec_doc.IsObject());
        ASSERT_TRUE(exec_doc.HasMember("resolved_sources") && exec_doc["resolved_sources"].IsArray());
        ASSERT_EQ(exec_doc["resolved_sources"].Size(), 2u);
        ASSERT_TRUE(exec_doc.HasMember("source_expand_rule") && exec_doc["source_expand_rule"].IsString());
        ASSERT_EQ(std::string(exec_doc["source_expand_rule"].GetString()), "auto_wildcard");

        std::string consumer_task_id = ParseTaskId(rsp);
        ASSERT_TRUE(!consumer_task_id.empty());
        std::string status_rsp;
        ASSERT_EQ(stream_status("/scheduler/stream/status", MakeTaskReq(consumer_task_id), status_rsp), error::OK);
        rapidjson::Document status_doc;
        status_doc.Parse(status_rsp.c_str());
        ASSERT_TRUE(!status_doc.HasParseError() && status_doc.IsObject());
        ASSERT_TRUE(status_doc.HasMember("resolved_sources") && status_doc["resolved_sources"].IsArray());
        ASSERT_EQ(status_doc["resolved_sources"].Size(), 2u);
        ASSERT_TRUE(status_doc.HasMember("source_expand_rule") && status_doc["source_expand_rule"].IsString());
        ASSERT_EQ(std::string(status_doc["source_expand_rule"].GetString()), "auto_wildcard");

        ASSERT_EQ(stream_stop("/scheduler/stream/stop", MakeTaskReq(consumer_task_id), status_rsp), error::OK);
        status_doc.Parse(status_rsp.c_str());
        ASSERT_TRUE(!status_doc.HasParseError() && status_doc.IsObject());
        ASSERT_TRUE(status_doc.HasMember("resolved_sources") && status_doc["resolved_sources"].IsArray());
        ASSERT_EQ(status_doc["resolved_sources"].Size(), 2u);

        std::string query_rsp;
        auto stream_query = FindRouteHandler(loader, "POST", "/channels/stream/query");
        ASSERT_TRUE(stream_query != nullptr);
        ASSERT_EQ(stream_query("/channels/stream/query", "{}", query_rsp), error::OK);
        rapidjson::Document qdoc;
        qdoc.Parse(query_rsp.c_str());
        ASSERT_TRUE(!qdoc.HasParseError() && qdoc.IsObject());
        ASSERT_TRUE(qdoc.HasMember("channels") && qdoc["channels"].IsArray());
        bool found_split_hub = false;
        bool found_merge_hub = false;
        for (const auto& item : qdoc["channels"].GetArray()) {
            if (!item.IsObject()) continue;
            if (!item.HasMember("name") || !item["name"].IsString()) continue;
            const std::string name = item["name"].GetString();
            if (name == "npm_hub") {
                found_split_hub = true;
                ASSERT_TRUE(item.HasMember("role") && item["role"].IsString());
                ASSERT_EQ(std::string(item["role"].GetString()), "both");
                ASSERT_TRUE(item.HasMember("option_json") && item["option_json"].IsObject());
                ASSERT_TRUE(item.HasMember("derived_channels") && item["derived_channels"].IsArray());
                ASSERT_EQ(item["derived_channels"].Size(), 2u);
            }
            if (name == "npm_merge") {
                found_merge_hub = true;
                ASSERT_TRUE(item.HasMember("derived_channels") && item["derived_channels"].IsArray());
                ASSERT_EQ(item["derived_channels"].Size(), 0u);
            }
        }
        ASSERT_TRUE(found_split_hub);
        ASSERT_TRUE(found_merge_hub);
    }
    std::puts("[PASS] T46");

    // T47: Web 代理流式通道查询接口（严格语义：上游不可达时返回 UNAVAILABLE）
    {
        const std::filesystem::path web_db_path = std::filesystem::temp_directory_path() /
                                                  ("flowsql_s9_3_web_" + suffix + ".db");
        std::filesystem::remove(web_db_path);

        // 当前 e2e 用例未加载 Gateway/Router 网络服务，Web 代理请求应返回 UNAVAILABLE。
        std::string web_opt = "host=127.0.0.1;port=18081;db_path=" + web_db_path.string() +
                              ";gateway=127.0.0.1:59883";
        const char* web_libs[] = {"libflowsql_web.so"};
        const char* web_opts[] = {web_opt.c_str()};
        ASSERT_EQ(loader->Load(get_absolute_process_path(), web_libs, web_opts, 1), 0);

        fnRouterHandler web_stream_query = nullptr;
        fnRouterHandler web_stream_definitions_query = nullptr;
        web_stream_query = FindRouteHandler(loader, "POST", "/api/channels/stream/query");
        web_stream_definitions_query = FindRouteHandler(loader, "POST", "/api/channels/stream/definitions/query");
        ASSERT_TRUE(web_stream_query != nullptr);
        ASSERT_TRUE(web_stream_definitions_query != nullptr);

        std::string rsp;
        ASSERT_EQ(web_stream_query("/api/channels/stream/query", "{}", rsp), error::UNAVAILABLE);
        ASSERT_TRUE(rsp.find("service unreachable") != std::string::npos);
        ASSERT_EQ(web_stream_definitions_query("/api/channels/stream/definitions/query", "{}", rsp), error::UNAVAILABLE);
        ASSERT_TRUE(rsp.find("service unreachable") != std::string::npos);
        std::filesystem::remove(web_db_path);
    }
    std::puts("[PASS] T47");

    exec = fnRouterHandler();
    stream_exec = fnRouterHandler();
    stream_stop = fnRouterHandler();
    stream_status = fnRouterHandler();
    stream_list = fnRouterHandler();
    stream_add = fnRouterHandler();
    stream_definitions_query = fnRouterHandler();
    sql_classify = fnRouterHandler();
    activate = fnRouterHandler();
    deactivate = fnRouterHandler();
    upsert_batch = fnRouterHandler();
    loader->StopAll();
    loader->Unload();
    std::filesystem::remove(db_path);
    std::filesystem::remove(stream_cfg);
    std::filesystem::remove(stream_meta_db);
    std::filesystem::remove_all(data_dir);
    std::filesystem::remove_all(operator_db_dir);

    std::puts("=== All Scheduler E2E tests passed ===");
    return 0;
}
