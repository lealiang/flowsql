#include <cstdio>
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <regex>
#ifndef _WIN32
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <framework/core/channel_adapter.h>
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/core/json_error_builder.h>
#include <framework/core/memory_channel.h>
#include <framework/builtin/dataframe/passthrough_operator.h>
#include <framework/core/pipeline.h>
#include <framework/core/sql_parser.h>
#include <framework/core/sql_text_splitter.h>
#include <framework/interfaces/ichannel.h>
#include <framework/interfaces/idataframe_channel.h>
#include <framework/interfaces/ioperator.h>
#include <rapidjson/document.h>

using namespace flowsql;

// Test declarations
void test_dataframe_basic();
void test_dataframe_arrow();
void test_dataframe_json();
void test_dataframe_clear();
void test_dataframe_channel();
void test_dataframe_channel_append();
void test_sql_parser();
void test_sql_text_splitter();
void test_operator_multi_input_fallback();
void test_span_safety();
void test_normalize_from_table_name();
void test_channel_adapter_copy();
void test_channel_type_constants();
void test_json_error_builder();
void test_pipeline();

// ============================================================
// Test 1: DataFrame 基本操作
// ============================================================
void test_dataframe_basic() {
    printf("[TEST] DataFrame basic operations...\n");
    DataFrame df;

    std::vector<Field> schema = {
        {"src_ip", DataType::STRING, 0, ""},
        {"dst_ip", DataType::STRING, 0, ""},
        {"bytes_sent", DataType::UINT64, 0, ""},
        {"protocol", DataType::STRING, 0, ""},
    };
    df.SetSchema(schema);

    df.AppendRow({std::string("192.168.1.1"), std::string("8.8.8.8"), uint64_t(1024), std::string("HTTP")});
    df.AppendRow({std::string("10.0.0.1"), std::string("172.16.0.1"), uint64_t(2048), std::string("DNS")});

    assert(df.RowCount() == 2);

    auto row0 = df.GetRow(0);
    assert(std::get<std::string>(row0[0]) == "192.168.1.1");
    assert(std::get<uint64_t>(row0[2]) == 1024);

    auto col = df.GetColumn("protocol");
    assert(col.size() == 2);
    assert(std::get<std::string>(col[0]) == "HTTP");
    assert(std::get<std::string>(col[1]) == "DNS");

    printf("[PASS] DataFrame basic operations\n");
}

// ============================================================
// Test 2: DataFrame Arrow 互操作
// ============================================================
void test_dataframe_arrow() {
    printf("[TEST] DataFrame Arrow interop...\n");
    DataFrame df1;
    std::vector<Field> schema = {
        {"id", DataType::INT32, 0, ""},
        {"name", DataType::STRING, 0, ""},
        {"score", DataType::DOUBLE, 0, ""},
    };
    df1.SetSchema(schema);
    df1.AppendRow({int32_t(1), std::string("Alice"), double(95.5)});
    df1.AppendRow({int32_t(2), std::string("Bob"), double(87.3)});

    auto batch = df1.ToArrow();
    assert(batch != nullptr);
    assert(batch->num_rows() == 2);
    assert(batch->num_columns() == 3);

    DataFrame df2;
    df2.FromArrow(batch);
    assert(df2.RowCount() == 2);
    auto row = df2.GetRow(1);
    assert(std::get<int32_t>(row[0]) == 2);
    assert(std::get<std::string>(row[1]) == "Bob");

    printf("[PASS] DataFrame Arrow interop\n");
}

// ============================================================
// Test 3: DataFrame JSON 序列化
// ============================================================
void test_dataframe_json() {
    printf("[TEST] DataFrame JSON serialization...\n");
    DataFrame df1;
    std::vector<Field> schema = {
        {"ip", DataType::STRING, 0, ""},
        {"port", DataType::UINT32, 0, ""},
        {"active", DataType::BOOLEAN, 0, ""},
    };
    df1.SetSchema(schema);
    df1.AppendRow({std::string("10.0.0.1"), uint32_t(8080), true});
    df1.AppendRow({std::string("10.0.0.2"), uint32_t(443), false});

    std::string json = df1.ToJson();
    assert(!json.empty());

    DataFrame df2;
    bool ok = df2.FromJson(json);
    assert(ok);
    assert(df2.RowCount() == 2);
    auto row = df2.GetRow(0);
    assert(std::get<std::string>(row[0]) == "10.0.0.1");
    assert(std::get<uint32_t>(row[1]) == 8080);
    assert(std::get<bool>(row[2]) == true);

    printf("[PASS] DataFrame JSON serialization\n");
}

// ============================================================
// Test 4: Pipeline 数据流通（直接构造 core 组件）
// ============================================================
void test_pipeline() {
    printf("[TEST] Pipeline run with MemoryChannel + PassthroughOperator...\n");

    MemoryChannel source;
    source.SetIdentity("test", "memory");
    PassthroughOperator op;

    DataFrame df_input;
    std::vector<Field> schema = {
        {"name", DataType::STRING, 0, ""},
        {"value", DataType::INT32, 0, ""},
    };
    df_input.SetSchema(schema);
    df_input.AppendRow({std::string("alpha"), int32_t(10)});
    df_input.AppendRow({std::string("beta"), int32_t(20)});

    source.Open();
    source.Write(&df_input);

    DataFrameChannel sink("test", "sink");
    sink.Open();

    auto pipeline = PipelineBuilder()
        .SetSource(&source)
        .SetOperator(&op)
        .SetSink(&sink)
        .Build();
    pipeline->Run();
    assert(pipeline->State() == PipelineState::STOPPED);

    DataFrame df_output;
    sink.Read(&df_output);
    assert(df_output.RowCount() == 2);

    auto row = df_output.GetRow(0);
    assert(std::get<std::string>(row[0]) == "alpha");
    assert(std::get<int32_t>(row[1]) == 10);

    auto row1 = df_output.GetRow(1);
    assert(std::get<std::string>(row1[0]) == "beta");
    assert(std::get<int32_t>(row1[1]) == 20);

    source.Close();
    sink.Close();

    printf("[PASS] Pipeline run with MemoryChannel + PassthroughOperator\n");
}

// ============================================================
// Test 5: DataFrame Clear + 重复使用
// ============================================================
void test_dataframe_clear() {
    printf("[TEST] DataFrame clear and reuse...\n");
    DataFrame df;
    std::vector<Field> schema = {{"val", DataType::INT32, 0, ""}};
    df.SetSchema(schema);
    df.AppendRow({int32_t(42)});
    assert(df.RowCount() == 1);

    df.Clear();
    assert(df.RowCount() == 0);

    df.AppendRow({int32_t(100)});
    assert(df.RowCount() == 1);
    assert(std::get<int32_t>(df.GetRow(0)[0]) == 100);

    printf("[PASS] DataFrame clear and reuse\n");
}

// ============================================================
// Test 6: DataFrameChannel 读写语义
// ============================================================
void test_dataframe_channel() {
    printf("[TEST] DataFrameChannel read/write semantics...\n");

    DataFrameChannel ch("test", "channel");
    ch.Open();

    DataFrame df1;
    df1.SetSchema({{"x", DataType::INT32, 0, ""}});
    df1.AppendRow({int32_t(1)});
    df1.AppendRow({int32_t(2)});
    ch.Write(&df1);

    DataFrame out1, out2;
    ch.Read(&out1);
    ch.Read(&out2);
    assert(out1.RowCount() == 2);
    assert(out2.RowCount() == 2);
    assert(std::get<int32_t>(out1.GetRow(0)[0]) == 1);
    assert(std::get<int32_t>(out2.GetRow(1)[0]) == 2);

    DataFrame df2;
    df2.SetSchema({{"y", DataType::STRING, 0, ""}});
    df2.AppendRow({std::string("hello")});
    ch.Write(&df2);

    DataFrame out3;
    ch.Read(&out3);
    assert(out3.RowCount() == 1);
    assert(std::get<std::string>(out3.GetRow(0)[0]) == "hello");

    assert(std::string(ch.Type()) == "dataframe");
    assert(std::string(ch.Category()) == "test");
    assert(std::string(ch.Name()) == "channel");

    ch.Close();
    printf("[PASS] DataFrameChannel read/write semantics\n");
}

// ============================================================
// Test 6.1: DataFrameChannel Append 追加语义
// ============================================================
void test_dataframe_channel_append() {
    printf("[TEST] DataFrameChannel append semantics...\n");

    DataFrameChannel ch("test", "append_channel");
    ch.Open();

    auto* appendable = dynamic_cast<IAppendableDataFrameChannel*>(&ch);
    assert(appendable != nullptr);

    DataFrame base;
    base.SetSchema({{"x", DataType::INT32, 0, ""}});
    base.AppendRow({int32_t(1)});
    base.AppendRow({int32_t(2)});
    assert(ch.Write(&base) == 0);

    DataFrame inc;
    inc.SetSchema({{"x", DataType::INT32, 0, ""}});
    inc.AppendRow({int32_t(3)});
    assert(appendable->Append(&inc) == 0);

    DataFrame out;
    assert(ch.Read(&out) == 0);
    assert(out.RowCount() == 3);
    assert(std::get<int32_t>(out.GetRow(0)[0]) == 1);
    assert(std::get<int32_t>(out.GetRow(1)[0]) == 2);
    assert(std::get<int32_t>(out.GetRow(2)[0]) == 3);

    DataFrame bad;
    bad.SetSchema({{"y", DataType::STRING, 0, ""}});
    bad.AppendRow({std::string("bad")});
    assert(appendable->Append(&bad) != 0);

    DataFrame out_after_bad;
    assert(ch.Read(&out_after_bad) == 0);
    assert(out_after_bad.RowCount() == 3);

    ch.Close();

    MemoryChannel mem;
    mem.SetIdentity("test", "append_memory");
    mem.Open();
    auto* mem_appendable = dynamic_cast<IAppendableDataFrameChannel*>(&mem);
    assert(mem_appendable != nullptr);

    DataFrame mem_base;
    mem_base.SetSchema({{"k", DataType::INT32, 0, ""}});
    mem_base.AppendRow({int32_t(10)});
    assert(mem.Write(&mem_base) == 0);

    DataFrame mem_inc;
    mem_inc.SetSchema({{"k", DataType::INT32, 0, ""}});
    mem_inc.AppendRow({int32_t(20)});
    assert(mem_appendable->Append(&mem_inc) == 0);

    DataFrame mem_out;
    assert(mem.Read(&mem_out) == 0);
    assert(mem_out.RowCount() == 2);
    assert(std::get<int32_t>(mem_out.GetRow(0)[0]) == 10);
    assert(std::get<int32_t>(mem_out.GetRow(1)[0]) == 20);
    mem.Close();

    printf("[PASS] DataFrameChannel append semantics\n");
}

// ============================================================
// Test 7: SQL 解析器基础测试
// ============================================================
void test_sql_parser() {
    printf("[TEST] SQL parser (USING optional + columns)...\n");
    SqlParser parser;

    {
        auto stmt = parser.Parse("SELECT * FROM test.data USING explore.chisquare WITH threshold=0.05 INTO result");
        assert(stmt.error.empty());
        assert(stmt.source == "test.data");
        assert(stmt.sources.size() == 1);
        assert(stmt.sources[0] == "test.data");
        assert(stmt.op_category == "explore");
        assert(stmt.op_name == "chisquare");
        assert(stmt.with_params["threshold"] == "0.05");
        assert(stmt.dest == "result");
        assert(stmt.columns.empty());
        assert(stmt.HasOperator());
    }

    {
        auto stmt = parser.Parse("SELECT * FROM memory_data INTO clickhouse.my_table");
        assert(stmt.error.empty());
        assert(stmt.source == "memory_data");
        assert(stmt.sources.size() == 1);
        assert(stmt.sources[0] == "memory_data");
        assert(!stmt.HasOperator());
        assert(stmt.dest == "clickhouse.my_table");
    }

    {
        auto stmt = parser.Parse("SELECT * FROM test.data");
        assert(stmt.error.empty());
        assert(stmt.source == "test.data");
        assert(stmt.sources.size() == 1);
        assert(stmt.sources[0] == "test.data");
        assert(!stmt.HasOperator());
        assert(stmt.dest.empty());
    }

    {
        auto stmt = parser.Parse("SELECT src_ip, dst_ip, bytes_sent FROM test.data USING explore.chisquare");
        assert(stmt.error.empty());
        assert(stmt.columns.size() == 3);
        assert(stmt.columns[0] == "src_ip");
        assert(stmt.HasOperator());
    }

    {
        auto stmt = parser.Parse("select * from test.data into result");
        assert(stmt.error.empty());
        assert(stmt.source == "test.data");
        assert(stmt.sources.size() == 1);
        assert(stmt.sources[0] == "test.data");
        assert(stmt.dest == "result");
    }

    // Test multi-source FROM parsing
    {
        auto stmt = parser.Parse("SELECT * FROM dataframe.d1, dataframe.d2 USING builtin.concat INTO dataframe.dd");
        assert(stmt.error.empty());
        assert(stmt.source == "dataframe.d1");
        assert(stmt.sources.size() == 2);
        assert(stmt.sources[0] == "dataframe.d1");
        assert(stmt.sources[1] == "dataframe.d2");
        assert(stmt.op_category == "builtin");
        assert(stmt.op_name == "concat");
        assert(stmt.dest == "dataframe.dd");
        assert(stmt.sql_part == "SELECT * FROM dataframe.d1, dataframe.d2");
    }

    // Test multi-source parser error: missing source after FROM
    {
        auto stmt = parser.Parse("SELECT * FROM");
        assert(stmt.error == "expected source channel name after FROM");
    }

    // Test multi-source parser error: missing source after comma
    {
        auto stmt = parser.Parse("SELECT * FROM dataframe.d1,");
        assert(stmt.error == "expected source channel name after ','");
    }

    // Test sql_part extraction with GROUP BY/ORDER BY (Story 4.5)
    {
        auto stmt = parser.Parse("SELECT a, COUNT(*) FROM source GROUP BY a ORDER BY COUNT(*) DESC USING ml.predict");
        assert(stmt.error.empty());
        assert(stmt.sql_part == "SELECT a, COUNT(*) FROM source GROUP BY a ORDER BY COUNT(*) DESC");
        assert(stmt.op_category == "ml");
        assert(stmt.op_name == "predict");
        assert(stmt.HasOperator());
    }

    // Test sql_part extraction without extension
    {
        auto stmt = parser.Parse("SELECT a FROM source WHERE x>1 GROUP BY a");
        assert(stmt.error.empty());
        assert(stmt.sql_part == "SELECT a FROM source WHERE x>1 GROUP BY a");
        assert(!stmt.HasOperator());
    }

    // Test sql_part extraction with WITH clause
    {
        auto stmt = parser.Parse("SELECT a, b FROM source WHERE x>1 WITH threshold=0.5 INTO dest");
        assert(stmt.error.empty());
        assert(stmt.sql_part == "SELECT a, b FROM source WHERE x>1");
        assert(stmt.with_params["threshold"] == "0.5");
        assert(stmt.dest == "dest");
    }

    // Test sql_part extraction with all clauses
    {
        auto stmt = parser.Parse("SELECT a, COUNT(*) FROM source WHERE x>1 GROUP BY a HAVING COUNT(*)>5 ORDER BY a LIMIT 10 USING ml.train WITH epochs=100 INTO result");
        assert(stmt.error.empty());
        assert(stmt.sql_part == "SELECT a, COUNT(*) FROM source WHERE x>1 GROUP BY a HAVING COUNT(*)>5 ORDER BY a LIMIT 10");
        assert(stmt.op_category == "ml");
        assert(stmt.op_name == "train");
        assert(stmt.with_params["epochs"] == "100");
        assert(stmt.dest == "result");
    }

    // Test THEN pipeline with per-operator WITH
    {
        auto stmt = parser.Parse(
            "SELECT * FROM source USING builtin.op1 WITH p1=1,p2=2 THEN ml.op2 WITH p3=3 INTO result");
        assert(stmt.error.empty());
        assert(stmt.operators.size() == 2);
        assert(stmt.operator_with_params.size() == 2);
        assert(stmt.operators[0].category == "builtin");
        assert(stmt.operators[0].name == "op1");
        assert(stmt.operator_with_params[0]["p1"] == "1");
        assert(stmt.operator_with_params[0]["p2"] == "2");
        assert(stmt.operators[1].category == "ml");
        assert(stmt.operators[1].name == "op2");
        assert(stmt.operator_with_params[1]["p3"] == "3");
    }

    // Test pipeline rejects global WITH after USING/THEN chain
    {
        auto stmt = parser.Parse("SELECT * FROM source USING builtin.op1 THEN builtin.op2 WITH p=1 INTO result");
        assert(stmt.error.empty());
        auto bad = parser.Parse("SELECT * FROM source USING builtin.op1 THEN builtin.op2 WITH p=1 WITH q=2 INTO result");
        assert(!bad.error.empty());
    }

    // Test stream selector syntax (Story 14.12)
    {
        auto stmt = parser.Parse(
            "SELECT * FROM stream.npm_hub[*], stream.npm_hub[0] "
            "USING builtin.passthrough_stream INTO stream.npm_sink");
        assert(stmt.error.empty());
        assert(stmt.sources.size() == 2);
        assert(stmt.sources[0] == "stream.npm_hub[*]");
        assert(stmt.sources[1] == "stream.npm_hub[0]");
        assert(stmt.dest == "stream.npm_sink");
    }

    // Test selector parse errors
    {
        auto bad = parser.Parse("SELECT * FROM stream.npm_hub[a] INTO dataframe.out");
        assert(!bad.error.empty());
        auto bad2 = parser.Parse("SELECT * FROM stream.npm_hub[ INTO dataframe.out");
        assert(!bad2.error.empty());
        auto bad3 = parser.Parse("SELECT * FROM stream.npm_hub[0][1] INTO dataframe.out");
        assert(!bad3.error.empty());
        auto ok = parser.Parse("SELECT * FROM stream.npm_hub INTO stream.npm_out[0]");
        assert(ok.error.empty());
        assert(ok.dest == "stream.npm_out[0]");
    }

    printf("[PASS] SQL parser (USING optional + columns)\n");
}

// ============================================================
// Test 7.1: sql_text splitter（Story 14.13）
// ============================================================
void test_sql_text_splitter() {
    printf("[TEST] sql_text splitter...\n");

    {
        std::vector<std::string> sqls;
        SqlTextSplitError err;
        assert(SplitSqlText("SELECT 1; SELECT 2;", &sqls, &err) == 0);
        assert(sqls.size() == 2);
        assert(sqls[0] == "SELECT 1");
        assert(sqls[1] == "SELECT 2");
    }

    {
        std::vector<std::string> sqls;
        SqlTextSplitError err;
        assert(SplitSqlText("SELECT 1\nSELECT 2", &sqls, &err) == 0);
        assert(sqls.size() == 1);
        assert(sqls[0] == "SELECT 1\nSELECT 2");
    }

    {
        std::vector<std::string> sqls;
        SqlTextSplitError err;
        const std::string text =
            "SELECT ';' AS a, \"x;\" AS b, `c;d` AS c;\n"
            "SELECT 2;";
        assert(SplitSqlText(text, &sqls, &err) == 0);
        assert(sqls.size() == 2);
    }

    {
        std::vector<std::string> sqls;
        SqlTextSplitError err;
        const std::string text =
            "SELECT 1 -- ; in line comment\n"
            ";\n"
            "SELECT /* ; in block comment */ 2;";
        assert(SplitSqlText(text, &sqls, &err) == 0);
        assert(sqls.size() == 2);
    }

    {
        std::vector<std::string> sqls;
        SqlTextSplitError err;
        assert(SplitSqlText("SELECT 1;;SELECT 2", &sqls, &err) != 0);
        assert(err.statement_index == 1);
    }

    {
        std::vector<std::string> sqls;
        SqlTextSplitError err;
        assert(SplitSqlText("SELECT 'abc; SELECT 2", &sqls, &err) != 0);
        assert(err.statement_index == 0);
    }

    printf("[PASS] sql_text splitter\n");
}

// ============================================================
// Test 8: IOperator 多输入默认回退（Span -> inputs[0]）
// ============================================================
void test_operator_multi_input_fallback() {
    printf("[TEST] IOperator multi-input fallback...\n");

    class SingleOnlyOperator : public IOperator {
     public:
        std::string Category() override { return "test"; }
        std::string Name() override { return "single_only"; }
        std::string Description() override { return "single input only"; }
        OperatorPosition Position() override { return OperatorPosition::DATA; }
        int Work(IChannel* in, IChannel* out) override {
            last_in = in;
            last_out = out;
            ++work_count;
            return 0;
        }
        int Configure(const char*, const char*) override { return 0; }

        IChannel* last_in = nullptr;
        IChannel* last_out = nullptr;
        int work_count = 0;
    };

    MemoryChannel in1;
    MemoryChannel in2;
    DataFrameChannel out("test", "sink");
    in1.SetIdentity("test", "in1");
    in2.SetIdentity("test", "in2");

    SingleOnlyOperator op;
    IOperator& op_iface = op;
    std::vector<IChannel*> inputs = {&in1, &in2};
    int rc = op_iface.Work(Span<IChannel*>(inputs), &out);
    assert(rc == 0);
    assert(op.work_count == 1);
    assert(op.last_in == &in1);
    assert(op.last_out == &out);

    std::vector<IChannel*> empty_inputs;
    rc = op_iface.Work(Span<IChannel*>(empty_inputs), &out);
    assert(rc != 0);

    printf("[PASS] IOperator multi-input fallback\n");
}

// ============================================================
// Test 9: Span 基础语义（empty + 越界 assert）
// ============================================================
void test_span_safety() {
    printf("[TEST] Span safety...\n");

    std::vector<int> empty_data;
    Span<int> empty_span(empty_data);
    assert(empty_span.empty());

    std::vector<int> one = {7};
    Span<int> one_span(one);
    assert(!one_span.empty());
    assert(one_span[0] == 7);

#ifndef _WIN32
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        volatile int v = one_span[1];
        (void)v;
        _exit(0);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGABRT);
#endif

    printf("[PASS] Span safety\n");
}

// ============================================================
// Test 8: NormalizeFromTableName (Story 4.5)
//
// 注意（TQ-8）：scheduler_plugin.cpp 中的 ExtractTableName/BuildQuery 是
// static 函数，无法从测试直接调用。这里复制了等价实现，并通过对比注释
// 确保与生产代码逻辑完全一致。如后续重构将其提取为公开函数，应改为直接调用。
// ============================================================

// 与 scheduler_plugin.cpp ExtractTableName 逻辑完全一致
static std::string ExtractTableName_Test(const std::string& dest_name) {
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

void test_normalize_from_table_name() {
    printf("[TEST] ExtractTableName (NormalizeFromTableName)...\n");

    // 验证 ExtractTableName_Test 与生产代码 ExtractTableName 逻辑一致
    assert(ExtractTableName_Test("sqlite.mydb.users") == "users");       // 三段式
    assert(ExtractTableName_Test("mydb.users") == "users");              // 两段式
    assert(ExtractTableName_Test("users") == "users");                   // 一段式
    assert(ExtractTableName_Test("catalog.db.table") == "table");        // 三段式
    assert(ExtractTableName_Test("a.b") == "b");                         // 两段式边界
    printf("  ExtractTableName: all cases OK\n");
    printf("[PASS] ExtractTableName (NormalizeFromTableName)\n");
}

// ============================================================
// Test 9: BuildQuery integration test (Story 4.5)
//
// 注意（TQ-9）：scheduler_plugin.cpp 中的 BuildQuery 是 static 函数，
// 无法从测试直接调用。这里复制了等价实现，逻辑与生产代码完全一致。
// 如后续重构将其提取为公开函数，应改为直接调用。
// ============================================================

// 与 scheduler_plugin.cpp BuildQuery 逻辑完全一致
static std::string BuildQuery_Test(const std::string& source_name, const SqlStatement& stmt) {
    std::string sql = stmt.sql_part;
    std::string table = ExtractTableName_Test(source_name);
    std::regex FROM_PATTERN(R"((\bFROM\s+)((?:[\w]+\.)*[\w]+))");
    std::smatch m;
    if (std::regex_search(sql, m, FROM_PATTERN)) {
        sql = sql.substr(0, m.position()) + m[1].str() + table +
              sql.substr(m.position() + m.length());
    }
    return sql;
}

void test_build_query_integration() {
    printf("[TEST] BuildQuery integration (Story 4.5)...\n");
    SqlParser parser;

    // Test 1: Database channel with GROUP BY/ORDER BY
    // sql_part 保留原始表名（不做占位符替换），BuildQuery 负责替换
    {
        auto stmt = parser.Parse("SELECT a, COUNT(*) FROM sqlite.mydb.logs GROUP BY a ORDER BY COUNT(*) DESC USING ml.predict");
        assert(stmt.error.empty());
        assert(stmt.sql_part == "SELECT a, COUNT(*) FROM sqlite.mydb.logs GROUP BY a ORDER BY COUNT(*) DESC");

        std::string query = BuildQuery_Test("sqlite.mydb.logs", stmt);
        assert(query == "SELECT a, COUNT(*) FROM logs GROUP BY a ORDER BY COUNT(*) DESC");
        printf("  [PASS] Database channel with GROUP BY/ORDER BY\n");
    }

    // Test 2: Database channel with subquery
    // 只替换第一个 FROM（主查询），子查询 FROM 保持原样
    {
        auto stmt = parser.Parse("SELECT * FROM sqlite.mydb.users WHERE id IN (SELECT user_id FROM sqlite.mydb.orders)");
        assert(stmt.error.empty());
        assert(stmt.sql_part == "SELECT * FROM sqlite.mydb.users WHERE id IN (SELECT user_id FROM sqlite.mydb.orders)");

        std::string query = BuildQuery_Test("sqlite.mydb.users", stmt);
        // 只替换主查询的 FROM，子查询 FROM sqlite.mydb.orders 不变
        assert(query == "SELECT * FROM users WHERE id IN (SELECT user_id FROM sqlite.mydb.orders)");
        printf("  [PASS] Database channel with subquery (first FROM only)\n");
    }

    // Test 3: DataFrame channel (sql_part should contain full SQL without USING)
    {
        auto stmt = parser.Parse("SELECT a, b FROM dataframe_source WHERE x > 1 AND y < 10");
        assert(stmt.error.empty());
        assert(stmt.sql_part == "SELECT a, b FROM dataframe_source WHERE x > 1 AND y < 10");
        assert(stmt.where_clause == "x > 1 AND y < 10");
        printf("  [PASS] DataFrame channel sql_part and where_clause\n");
    }

    // Test 4: Full SQL with all clauses
    {
        auto stmt = parser.Parse("SELECT a, COUNT(*) FROM catalog.db.table WHERE x > 1 GROUP BY a HAVING COUNT(*) > 5 ORDER BY a LIMIT 10 USING ml.train WITH epochs=100 INTO result");
        assert(stmt.error.empty());
        assert(stmt.sql_part == "SELECT a, COUNT(*) FROM catalog.db.table WHERE x > 1 GROUP BY a HAVING COUNT(*) > 5 ORDER BY a LIMIT 10");
        assert(stmt.op_category == "ml");
        assert(stmt.op_name == "train");
        assert(stmt.with_params["epochs"] == "100");
        assert(stmt.dest == "result");

        std::string query = BuildQuery_Test("catalog.db.table", stmt);
        assert(query == "SELECT a, COUNT(*) FROM table WHERE x > 1 GROUP BY a HAVING COUNT(*) > 5 ORDER BY a LIMIT 10");
        printf("  [PASS] Full SQL with all clauses\n");
    }

    printf("[PASS] BuildQuery integration\n");
}

// ============================================================
// Test 11: ChannelType 常量验证（P3-3 修复）
// ============================================================
void test_channel_type_constants() {
    printf("[TEST] ChannelType constants...\n");
    assert(std::string(ChannelType::kDataFrame) == "dataframe");
    assert(std::string(ChannelType::kDatabase) == "database");

    // DataFrameChannel 应返回 kDataFrame
    DataFrameChannel ch("test", "t");
    assert(std::string(ch.Type()) == ChannelType::kDataFrame);

    printf("[PASS] ChannelType constants\n");
}

// ============================================================
// Test 10: ChannelAdapter — DataFrame 搬运
// ============================================================
void test_channel_adapter_copy() {
    printf("[TEST] ChannelAdapter CopyDataFrame...\n");

    DataFrameChannel src("test", "src");
    DataFrameChannel dst("test", "dst");
    src.Open();
    dst.Open();

    DataFrame df;
    df.SetSchema({{"x", DataType::INT32, 0, ""}, {"y", DataType::STRING, 0, ""}});
    df.AppendRow({int32_t(1), std::string("hello")});
    df.AppendRow({int32_t(2), std::string("world")});
    src.Write(&df);

    int rc = ChannelAdapter::CopyDataFrame(&src, &dst);
    assert(rc == 0);

    DataFrame result;
    dst.Read(&result);
    assert(result.RowCount() == 2);
    assert(std::get<int32_t>(result.GetRow(0)[0]) == 1);
    assert(std::get<std::string>(result.GetRow(1)[1]) == "world");

    src.Close();
    dst.Close();
    printf("[PASS] ChannelAdapter CopyDataFrame\n");
}

// ============================================================
// Test 12: JSON Error Builder（Sprint15 P2-2）
// ============================================================
void test_json_error_builder() {
    printf("[TEST] JSON error builder...\n");

    {
        rapidjson::Document d;
        const std::string json = BuildErrorJson("simple");
        d.Parse(json.c_str());
        assert(!d.HasParseError() && d.IsObject());
        assert(d.HasMember("error") && d["error"].IsString());
        assert(std::string(d["error"].GetString()) == "simple");
        assert(!d.HasMember("error_code"));
    }

    {
        rapidjson::Document d;
        const std::string json = BuildExecutionErrorWithSqlIndexJson(
            "exec failed", "OP_EXEC_FAIL", "execute", 3);
        d.Parse(json.c_str());
        assert(!d.HasParseError() && d.IsObject());
        assert(std::string(d["error"].GetString()) == "exec failed");
        assert(std::string(d["error_code"].GetString()) == "OP_EXEC_FAIL");
        assert(std::string(d["error_stage"].GetString()) == "execute");
        assert(d["sql_index"].IsUint64() && d["sql_index"].GetUint64() == 3);
    }

    {
        StreamChannelCapabilities source_caps;
        source_caps.channel_type = "ring";
        source_caps.concurrency.put_mode = ProducerMode::SINGLE;
        source_caps.concurrency.poll_mode = ConsumerMode::MULTI;
        source_caps.concurrency.max_producers = 1;
        source_caps.concurrency.max_consumers = 4;
        source_caps.concurrency.lock_free_put = true;
        source_caps.concurrency.lock_free_poll = true;
        source_caps.concurrency.cancel_wakeup_guaranteed = true;

        StreamChannelCapabilities sink_caps;
        sink_caps.channel_type = "ring";
        sink_caps.concurrency.put_mode = ProducerMode::MULTI;
        sink_caps.concurrency.poll_mode = ConsumerMode::SINGLE;
        sink_caps.concurrency.max_producers = 8;
        sink_caps.concurrency.max_consumers = 1;
        sink_caps.concurrency.lock_free_put = true;
        sink_caps.concurrency.lock_free_poll = false;
        sink_caps.concurrency.cancel_wakeup_guaranteed = true;

        rapidjson::Document d;
        const std::string json = BuildCapabilityMismatchJson(
            "cap mismatch", "STREAM_SOURCE_CAPABILITY_MISMATCH", &source_caps, &sink_caps);
        d.Parse(json.c_str());
        assert(!d.HasParseError() && d.IsObject());
        assert(std::string(d["error_stage"].GetString()) == "capability_check");
        assert(d.HasMember("details") && d["details"].IsObject());
        assert(d["details"].HasMember("capabilities") && d["details"]["capabilities"].IsObject());
        assert(d["details"]["capabilities"].HasMember("source"));
        assert(d["details"]["capabilities"].HasMember("sink"));
    }

    {
        rapidjson::Document d;
        const std::string json = BuildSinkCapabilityMismatchJson(
            "sink mismatch", "capability_check", "stream.out", 3, ProducerMode::SINGLE, 1);
        d.Parse(json.c_str());
        assert(!d.HasParseError() && d.IsObject());
        assert(std::string(d["error_code"].GetString()) == "STREAM_GROUP_SINK_CAPABILITY_MISMATCH");
        assert(std::string(d["sink_key"].GetString()) == "stream.out");
        assert(d["required"].IsObject());
        assert(d["required"]["writers"].GetUint() == 3);
        assert(d["actual"].IsObject());
        assert(std::string(d["actual"]["put_mode"].GetString()) == "SINGLE");
    }

    {
        rapidjson::Document d;
        const std::vector<std::string> expected = {"stream.a", "stream.c"};
        const std::vector<std::string> actual = {"stream.d", "stream.c"};
        const std::string json = BuildSourceMismatchErrorJson(
            "source mismatch", "share_set_validate", "s1", "node1", expected, actual);
        d.Parse(json.c_str());
        assert(!d.HasParseError() && d.IsObject());
        assert(std::string(d["error_code"].GetString()) == "STREAM_GROUP_SOURCE_MISMATCH");
        assert(d["missing_keys"].IsArray());
        assert(d["missing_keys"].Size() == 1);
        assert(std::string(d["missing_keys"][0].GetString()) == "stream.a");
        assert(d["extra_keys"].IsArray());
        assert(d["extra_keys"].Size() == 1);
        assert(std::string(d["extra_keys"][0].GetString()) == "stream.d");
    }

    printf("[PASS] JSON error builder\n");
}

// ============================================================
// main
// ============================================================
int main(int argc, char* argv[]) {
    printf("=== FlowSQL Framework Tests ===\n\n");

    test_dataframe_basic();
    test_dataframe_arrow();
    test_dataframe_json();
    test_dataframe_clear();
    test_dataframe_channel();
    test_dataframe_channel_append();
    test_sql_parser();
    test_sql_text_splitter();
    test_operator_multi_input_fallback();
    test_span_safety();
    test_normalize_from_table_name();
    test_build_query_integration();
    test_channel_adapter_copy();
    test_channel_type_constants();
    test_json_error_builder();

    test_pipeline();

    printf("\n=== All tests passed ===\n");
    return 0;
}
