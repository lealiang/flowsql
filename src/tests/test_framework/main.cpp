#include <cstdio>
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <common/toolkit.hpp>
#include <framework/core/dataframe.h>
#include <framework/core/pipeline.h>
#include <framework/core/plugin_registry.h>
#include <framework/interfaces/ichannel.h>
#include <framework/interfaces/ioperator.h>

using namespace flowsql;

// ============================================================
// Test 1: DataFrame 基本操作
// ============================================================
void test_dataframe_basic() {
    printf("[TEST] DataFrame basic operations...\n");
    DataFrame df;

    // SetSchema
    std::vector<Field> schema = {
        {"src_ip", DataType::STRING, 0, ""},
        {"dst_ip", DataType::STRING, 0, ""},
        {"bytes_sent", DataType::UINT64, 0, ""},
        {"protocol", DataType::STRING, 0, ""},
    };
    df.SetSchema(schema);

    // AppendRow
    df.AppendRow({std::string("192.168.1.1"), std::string("8.8.8.8"), uint64_t(1024), std::string("HTTP")});
    df.AppendRow({std::string("10.0.0.1"), std::string("172.16.0.1"), uint64_t(2048), std::string("DNS")});

    assert(df.RowCount() == 2);

    // GetRow
    auto row0 = df.GetRow(0);
    assert(std::get<std::string>(row0[0]) == "192.168.1.1");
    assert(std::get<uint64_t>(row0[2]) == 1024);

    // GetColumn
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

    // ToArrow
    auto batch = df1.ToArrow();
    assert(batch != nullptr);
    assert(batch->num_rows() == 2);
    assert(batch->num_columns() == 3);

    // FromArrow (零拷贝)
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

    // FromJson
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
// Test 4: 插件加载 + Pipeline 数据流通
// ============================================================
void test_pipeline(const std::string& plugin_dir) {
    printf("[TEST] Plugin loading + Pipeline...\n");

    PluginRegistry registry;

    // PluginLoader::Load 会自动拼接 get_absolute_process_path() 前缀
    // 所以只传相对于可执行文件目录的文件名
    std::string plugin_name = "libflowsql_example.so";
    int ret = registry.LoadPlugin(plugin_name);
    if (ret != 0) {
        printf("[SKIP] Plugin not found: %s, skipping pipeline test\n", plugin_name.c_str());
        return;
    }

    // 查询插件
    IChannel* source = registry.GetChannel("example", "memory");
    IOperator* op = registry.GetOperator("example", "passthrough");
    assert(source != nullptr);
    assert(op != nullptr);

    // 完整 Pipeline 数据流通测试
    source->Open();

    // 创建一个 sink channel（复用同一个 MemoryChannel 类型，但这里用 source 做双向测试）
    // 构造测试数据：通过 GenericDataEntity 写入 source
    DataFrame df_input;
    std::vector<Field> schema = {
        {"name", DataType::STRING, 0, ""},
        {"value", DataType::INT32, 0, ""},
    };
    df_input.SetSchema(schema);
    df_input.AppendRow({std::string("alpha"), int32_t(10)});
    df_input.AppendRow({std::string("beta"), int32_t(20)});

    // 将 DataFrame 行写入 source channel
    for (int32_t i = 0; i < df_input.RowCount(); ++i) {
        auto entity = df_input.GetEntity(i);
        source->Put(entity.get());
    }

    // 构建 Pipeline：source → passthrough → (无 sink，只验证算子输出)
    DataFrame in_frame, out_frame;
    for (int i = 0; i < 10; ++i) {
        IDataEntity* entity = source->Get();
        if (!entity) break;
        in_frame.AppendEntity(entity);
    }
    assert(in_frame.RowCount() == 2);

    int work_ret = op->Work(&in_frame, &out_frame);
    assert(work_ret == 0);
    assert(out_frame.RowCount() == 2);

    auto row = out_frame.GetRow(0);
    assert(std::get<std::string>(row[0]) == "alpha");
    assert(std::get<int32_t>(row[1]) == 10);

    source->Close();
    registry.UnloadAll();

    printf("[PASS] Plugin loading + Pipeline\n");
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
// main
// ============================================================
int main(int argc, char* argv[]) {
    printf("=== FlowSQL Framework Tests ===\n\n");

    test_dataframe_basic();
    test_dataframe_arrow();
    test_dataframe_json();
    test_dataframe_clear();

    // Pipeline 测试需要插件 .so
    std::string plugin_dir = get_absolute_process_path();
    test_pipeline(plugin_dir);

    printf("\n=== All tests passed ===\n");
    return 0;
}
