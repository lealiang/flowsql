#include <cstdio>
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <common/toolkit.hpp>
#include <framework/core/channel_adapter.h>
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/core/pipeline.h>
#include <framework/core/plugin_registry.h>
#include <framework/core/sql_parser.h>
#include <framework/interfaces/ichannel.h>
#include <framework/interfaces/idataframe_channel.h>
#include <framework/interfaces/ioperator.h>

using namespace flowsql;

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

// PLACEHOLDER_REST

// ============================================================
// Test 4: 插件加载 + Pipeline 数据流通（新接口：IDataFrameChannel + Work(IChannel*, IChannel*)）
// ============================================================
void test_pipeline(const std::string& plugin_dir) {
    printf("[TEST] Plugin loading + Pipeline (new interface)...\n");

    PluginRegistry* registry = PluginRegistry::Instance();

    std::string plugin_name = "libflowsql_example.so";
    int ret = registry->LoadPlugin(plugin_name);
    if (ret != 0) {
        printf("[SKIP] Plugin not found: %s, skipping pipeline test\n", plugin_name.c_str());
        return;
    }

    // 查询插件
    IChannel* source_ch = registry->Get<IChannel>(IID_CHANNEL, "example.memory");
    IOperator* op = registry->Get<IOperator>(IID_OPERATOR, "example.passthrough");
    assert(source_ch != nullptr);
    assert(op != nullptr);

    // 确认是 IDataFrameChannel
    auto* df_source = dynamic_cast<IDataFrameChannel*>(source_ch);
    assert(df_source != nullptr);

    // 准备测试数据
    DataFrame df_input;
    std::vector<Field> schema = {
        {"name", DataType::STRING, 0, ""},
        {"value", DataType::INT32, 0, ""},
    };
    df_input.SetSchema(schema);
    df_input.AppendRow({std::string("alpha"), int32_t(10)});
    df_input.AppendRow({std::string("beta"), int32_t(20)});

    // 写入 source channel
    df_source->Open();
    df_source->Write(&df_input);

    // 创建 sink channel（DataFrameChannel）
    DataFrameChannel sink("test", "sink");
    sink.Open();

    // Pipeline 执行：source → passthrough → sink
    auto pipeline = PipelineBuilder()
        .SetSource(df_source)
        .SetOperator(op)
        .SetSink(&sink)
        .Build();
    pipeline->Run();
    assert(pipeline->State() == PipelineState::STOPPED);

    // 从 sink 读取结果
    DataFrame df_output;
    sink.Read(&df_output);
    assert(df_output.RowCount() == 2);

    auto row = df_output.GetRow(0);
    assert(std::get<std::string>(row[0]) == "alpha");
    assert(std::get<int32_t>(row[1]) == 10);

    auto row1 = df_output.GetRow(1);
    assert(std::get<std::string>(row1[0]) == "beta");
    assert(std::get<int32_t>(row1[1]) == 20);

    df_source->Close();
    sink.Close();
    registry->UnloadAll();

    printf("[PASS] Plugin loading + Pipeline (new interface)\n");
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

// PLACEHOLDER_DYNAMIC

// ============================================================
// Test 6: 动态注册/注销 + 合并遍历
// ============================================================

// 测试用的简单 IOperator 实现
class MockDynamicOperator : public IOperator {
 public:
    MockDynamicOperator(std::string catelog, std::string name)
        : catelog_(std::move(catelog)), name_(std::move(name)) {}

    int Load() override { return 0; }
    int Unload() override { return 0; }
    std::string Catelog() override { return catelog_; }
    std::string Name() override { return name_; }
    std::string Description() override { return "dynamic test operator"; }
    OperatorPosition Position() override { return OperatorPosition::DATA; }
    int Work(IChannel*, IChannel*) override { return 0; }
    int Configure(const char*, const char*) override { return 0; }

 private:
    std::string catelog_;
    std::string name_;
};

void test_dynamic_register() {
    printf("[TEST] Dynamic register/unregister...\n");

    PluginRegistry* registry = PluginRegistry::Instance();

    std::string plugin_name = "libflowsql_example.so";
    int ret = registry->LoadPlugin(plugin_name);
    if (ret != 0) {
        printf("[SKIP] Plugin not found, skipping dynamic register test\n");
        return;
    }

    // 验证静态插件存在
    IOperator* static_op = registry->Get<IOperator>(IID_OPERATOR, "example.passthrough");
    assert(static_op != nullptr);

    // 动态注册一个新算子
    auto dyn_op = std::make_shared<MockDynamicOperator>("dynamic", "mock");
    registry->Register(IID_OPERATOR, "dynamic.mock", dyn_op);

    // 通过 Get 查询动态算子
    IOperator* found = registry->Get<IOperator>(IID_OPERATOR, "dynamic.mock");
    assert(found != nullptr);
    assert(found->Catelog() == "dynamic");
    assert(found->Name() == "mock");

    // 静态算子仍然可查
    assert(registry->Get<IOperator>(IID_OPERATOR, "example.passthrough") != nullptr);

    // 遍历合并：应该同时看到静态和动态算子
    int count = 0;
    bool found_static = false, found_dynamic = false;
    registry->Traverse(IID_OPERATOR, [&](void* p) -> int {
        auto* op = static_cast<IOperator*>(p);
        count++;
        if (op->Catelog() == "example" && op->Name() == "passthrough") found_static = true;
        if (op->Catelog() == "dynamic" && op->Name() == "mock") found_dynamic = true;
        return 0;
    });
    assert(count >= 2);
    assert(found_static);
    assert(found_dynamic);

    // 动态覆盖静态
    auto override_op = std::make_shared<MockDynamicOperator>("example", "passthrough_v2");
    registry->Register(IID_OPERATOR, "example.passthrough", override_op);

    IOperator* overridden = registry->Get<IOperator>(IID_OPERATOR, "example.passthrough");
    assert(overridden != nullptr);
    assert(overridden->Name() == "passthrough_v2");

    // 遍历时同 key 不重复
    int passthrough_count = 0;
    registry->Traverse(IID_OPERATOR, [&](void* p) -> int {
        auto* op = static_cast<IOperator*>(p);
        if (op->Catelog() == "example" &&
            (op->Name() == "passthrough" || op->Name() == "passthrough_v2")) {
            passthrough_count++;
        }
        return 0;
    });
    assert(passthrough_count == 1);

    // 注销动态覆盖
    registry->Unregister(IID_OPERATOR, "example.passthrough");
    IOperator* restored = registry->Get<IOperator>(IID_OPERATOR, "example.passthrough");
    assert(restored != nullptr);
    assert(restored->Name() == "passthrough");

    // 注销动态 mock
    registry->Unregister(IID_OPERATOR, "dynamic.mock");
    assert(registry->Get(IID_OPERATOR, "dynamic.mock") == nullptr);

    registry->UnloadAll();

    printf("[PASS] Dynamic register/unregister\n");
}

// PLACEHOLDER_DFCHANNEL

// ============================================================
// Test 7: DataFrameChannel 读写语义
// ============================================================
void test_dataframe_channel() {
    printf("[TEST] DataFrameChannel read/write semantics...\n");

    DataFrameChannel ch("test", "channel");
    ch.Open();

    // 写入数据
    DataFrame df1;
    df1.SetSchema({{"x", DataType::INT32, 0, ""}});
    df1.AppendRow({int32_t(1)});
    df1.AppendRow({int32_t(2)});
    ch.Write(&df1);

    // Read() 快照语义：多次读取结果一致
    DataFrame out1, out2;
    ch.Read(&out1);
    ch.Read(&out2);
    assert(out1.RowCount() == 2);
    assert(out2.RowCount() == 2);
    assert(std::get<int32_t>(out1.GetRow(0)[0]) == 1);
    assert(std::get<int32_t>(out2.GetRow(1)[0]) == 2);

    // Write() 替换语义：覆盖旧数据
    DataFrame df2;
    df2.SetSchema({{"y", DataType::STRING, 0, ""}});
    df2.AppendRow({std::string("hello")});
    ch.Write(&df2);

    DataFrame out3;
    ch.Read(&out3);
    assert(out3.RowCount() == 1);
    assert(std::get<std::string>(out3.GetRow(0)[0]) == "hello");

    // Type() 和 Schema()
    assert(std::string(ch.Type()) == "dataframe");
    assert(std::string(ch.Catelog()) == "test");
    assert(std::string(ch.Name()) == "channel");

    ch.Close();
    printf("[PASS] DataFrameChannel read/write semantics\n");
}

// ============================================================
// Test 8: SQL 解析器 — USING 可选 + 列选择
// ============================================================
void test_sql_parser() {
    printf("[TEST] SQL parser (USING optional + columns)...\n");
    SqlParser parser;

    // 1. 完整语法（兼容旧格式）
    {
        auto stmt = parser.Parse("SELECT * FROM test.data USING explore.chisquare WITH threshold=0.05 INTO result");
        assert(stmt.error.empty());
        assert(stmt.source == "test.data");
        assert(stmt.op_catelog == "explore");
        assert(stmt.op_name == "chisquare");
        assert(stmt.with_params["threshold"] == "0.05");
        assert(stmt.dest == "result");
        assert(stmt.columns.empty());  // SELECT *
        assert(stmt.HasOperator());
    }

    // 2. 无 USING（纯数据搬运）
    {
        auto stmt = parser.Parse("SELECT * FROM memory_data INTO clickhouse.my_table");
        assert(stmt.error.empty());
        assert(stmt.source == "memory_data");
        assert(stmt.op_catelog.empty());
        assert(stmt.op_name.empty());
        assert(stmt.dest == "clickhouse.my_table");
        assert(!stmt.HasOperator());
    }

    // 3. 无 USING 无 INTO（纯查询）
    {
        auto stmt = parser.Parse("SELECT * FROM test.data");
        assert(stmt.error.empty());
        assert(stmt.source == "test.data");
        assert(!stmt.HasOperator());
        assert(stmt.dest.empty());
    }

    // 4. 列选择
    {
        auto stmt = parser.Parse("SELECT src_ip, dst_ip, bytes_sent FROM test.data USING explore.chisquare");
        assert(stmt.error.empty());
        assert(stmt.columns.size() == 3);
        assert(stmt.columns[0] == "src_ip");
        assert(stmt.columns[1] == "dst_ip");
        assert(stmt.columns[2] == "bytes_sent");
        assert(stmt.source == "test.data");
        assert(stmt.HasOperator());
    }

    // 5. 无 USING + WITH（WITH 参数用于 Database 过滤）
    {
        auto stmt = parser.Parse("SELECT * FROM clickhouse.my_table WITH where=\"date > '2026-01-01'\" INTO memory_result");
        assert(stmt.error.empty());
        assert(stmt.source == "clickhouse.my_table");
        assert(!stmt.HasOperator());
        assert(stmt.with_params.count("where") == 1);
        assert(stmt.dest == "memory_result");
    }

    // 6. 大小写不敏感
    {
        auto stmt = parser.Parse("select * from test.data into result");
        assert(stmt.error.empty());
        assert(stmt.source == "test.data");
        assert(stmt.dest == "result");
    }

    printf("[PASS] SQL parser (USING optional + columns)\n");
}

// ============================================================
// Test 9: ChannelAdapter — DataFrame 搬运
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
// main
// ============================================================
int main(int argc, char* argv[]) {
    printf("=== FlowSQL Framework Tests ===\n\n");

    test_dataframe_basic();
    test_dataframe_arrow();
    test_dataframe_json();
    test_dataframe_clear();
    test_dataframe_channel();
    test_sql_parser();
    test_channel_adapter_copy();

    // Pipeline 测试需要插件 .so
    std::string plugin_dir = get_absolute_process_path();
    test_pipeline(plugin_dir);

    // 动态注册测试需要插件 .so
    test_dynamic_register();

    printf("\n=== All tests passed ===\n");
    return 0;
}
