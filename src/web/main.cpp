#include <cstdio>
#include <csignal>
#include <string>
#include <vector>

#include "framework/core/dataframe.h"
#include "framework/core/dataframe_channel.h"
#include "framework/core/plugin_registry.h"
#include "framework/interfaces/idataframe_channel.h"
#include "web/web_server.h"

using namespace flowsql;

static web::WebServer* g_server = nullptr;

void signal_handler(int) {
    if (g_server) g_server->Stop();
}

// 创建预填测试数据
static std::shared_ptr<DataFrameChannel> CreateTestData() {
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

    // 预填模拟网络流量数据
    df.AppendRow({std::string("192.168.1.10"), std::string("10.0.0.1"), uint32_t(52341), uint32_t(80),
                  std::string("HTTP"), uint64_t(1024), uint64_t(4096)});
    df.AppendRow({std::string("192.168.1.10"), std::string("8.8.8.8"), uint32_t(53421), uint32_t(53),
                  std::string("DNS"), uint64_t(64), uint64_t(128)});
    df.AppendRow({std::string("192.168.1.20"), std::string("172.16.0.5"), uint32_t(44312), uint32_t(443),
                  std::string("HTTPS"), uint64_t(2048), uint64_t(8192)});
    df.AppendRow({std::string("192.168.1.30"), std::string("10.0.0.1"), uint32_t(61234), uint32_t(80),
                  std::string("HTTP"), uint64_t(512), uint64_t(2048)});
    df.AppendRow({std::string("10.0.0.5"), std::string("192.168.1.10"), uint32_t(8080), uint32_t(55123),
                  std::string("HTTP"), uint64_t(4096), uint64_t(1024)});
    df.AppendRow({std::string("192.168.1.10"), std::string("172.16.0.10"), uint32_t(39876), uint32_t(22),
                  std::string("SSH"), uint64_t(256), uint64_t(512)});
    df.AppendRow({std::string("192.168.1.20"), std::string("8.8.4.4"), uint32_t(54321), uint32_t(53),
                  std::string("DNS"), uint64_t(48), uint64_t(96)});
    df.AppendRow({std::string("192.168.1.30"), std::string("10.0.0.2"), uint32_t(45678), uint32_t(3306),
                  std::string("MySQL"), uint64_t(128), uint64_t(1024)});
    df.AppendRow({std::string("192.168.1.10"), std::string("172.16.0.5"), uint32_t(41234), uint32_t(443),
                  std::string("HTTPS"), uint64_t(1536), uint64_t(6144)});
    df.AppendRow({std::string("10.0.0.1"), std::string("192.168.1.20"), uint32_t(80), uint32_t(62345),
                  std::string("HTTP"), uint64_t(8192), uint64_t(512)});
    df.AppendRow({std::string("192.168.1.20"), std::string("10.0.0.3"), uint32_t(37890), uint32_t(6379),
                  std::string("Redis"), uint64_t(64), uint64_t(256)});
    df.AppendRow({std::string("192.168.1.30"), std::string("8.8.8.8"), uint32_t(51234), uint32_t(53),
                  std::string("DNS"), uint64_t(56), uint64_t(112)});
    df.AppendRow({std::string("192.168.1.10"), std::string("10.0.0.1"), uint32_t(48765), uint32_t(80),
                  std::string("HTTP"), uint64_t(768), uint64_t(3072)});
    df.AppendRow({std::string("192.168.1.20"), std::string("172.16.0.5"), uint32_t(43210), uint32_t(443),
                  std::string("HTTPS"), uint64_t(1024), uint64_t(4096)});
    df.AppendRow({std::string("10.0.0.5"), std::string("192.168.1.30"), uint32_t(9090), uint32_t(56789),
                  std::string("HTTP"), uint64_t(2048), uint64_t(512)});
    df.AppendRow({std::string("192.168.1.10"), std::string("10.0.0.4"), uint32_t(39012), uint32_t(5432),
                  std::string("PostgreSQL"), uint64_t(256), uint64_t(2048)});
    df.AppendRow({std::string("192.168.1.30"), std::string("172.16.0.10"), uint32_t(42345), uint32_t(22),
                  std::string("SSH"), uint64_t(512), uint64_t(1024)});
    df.AppendRow({std::string("192.168.1.20"), std::string("10.0.0.1"), uint32_t(47890), uint32_t(80),
                  std::string("HTTP"), uint64_t(384), uint64_t(1536)});
    df.AppendRow({std::string("10.0.0.2"), std::string("192.168.1.10"), uint32_t(3306), uint32_t(58901),
                  std::string("MySQL"), uint64_t(1024), uint64_t(128)});
    df.AppendRow({std::string("192.168.1.30"), std::string("10.0.0.5"), uint32_t(46789), uint32_t(8080),
                  std::string("HTTP"), uint64_t(640), uint64_t(2560)});

    ch->Write(&df);
    return ch;
}

int main(int argc, char* argv[]) {
    printf("========================================\n");
    printf("  FlowSQL Web Management System\n");
    printf("========================================\n\n");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 1. PluginRegistry
    PluginRegistry* registry = PluginRegistry::Instance();

    // 2. 加载插件
    std::string example_plugin = "libflowsql_example.so";
    if (registry->LoadPlugin(example_plugin) == 0) {
        printf("Loaded plugin: %s\n", example_plugin.c_str());
    }

    // 3. 创建预填测试数据并注册
    auto test_data = CreateTestData();
    registry->Register(IID_CHANNEL, "test.data", test_data);
    registry->Register(IID_DATAFRAME_CHANNEL, "test.data", test_data);
    printf("Registered test_data channel (%d rows)\n", 20);

    // 4. 初始化 Web 服务器
    web::WebServer server;
    g_server = &server;

    std::string db_path = "/tmp/flowsql.db";
    if (server.Init(db_path, registry) != 0) {
        printf("Failed to init web server\n");
        return 1;
    }

    // 5. 启动监听
    printf("\nStarting server on http://0.0.0.0:8081\n\n");
    server.Start("0.0.0.0", 8081);

    return 0;
}
