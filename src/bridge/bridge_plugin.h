#ifndef _FLOWSQL_BRIDGE_BRIDGE_PLUGIN_H_
#define _FLOWSQL_BRIDGE_BRIDGE_PLUGIN_H_

#include <memory>
#include <string>
#include <vector>

#include <common/loader.hpp>

#include "control_server.h"
#include "python_operator_bridge.h"
#include "python_process_manager.h"

namespace flowsql {

class PluginRegistry;

namespace bridge {

// BridgePlugin — 桥接插件生命周期管理器
// 实现 IPlugin（Load/Unload）+ IModule（Start/Stop）+ IMessageHandler（处理 Worker 消息）
// 三阶段加载：pluginregist 注册 → Load 读配置 → Start 启动 Worker + 动态注册算子
class BridgePlugin : public IPlugin, public IModule, public IMessageHandler {
 public:
    BridgePlugin() = default;
    ~BridgePlugin() override = default;

    // IPlugin
    int Option(const char* arg) override;
    int Load() override;
    int Unload() override;

    // IModule
    int Start() override;
    int Stop() override;

    // IMessageHandler
    void OnWorkerReady(const std::vector<OperatorMeta>& operators) override;
    void OnOperatorAdded(const OperatorMeta& meta) override;
    void OnOperatorRemoved(const std::string& catelog, const std::string& name) override;
    void OnHeartbeat(const std::string& stats_json) override;
    void OnError(int code, const std::string& message) override;

 private:
    PythonProcessManager process_manager_;
    PluginRegistry* registry_ = nullptr;

    // 已动态注册的算子 key 列表（catelog.name）
    std::vector<std::string> registered_keys_;

    // 配置参数
    std::string python_path_ = "python3";
    std::string host_ = "127.0.0.1";
    int port_ = 18900;
    std::string operators_dir_;
};

}  // namespace bridge
}  // namespace flowsql

#endif  // _FLOWSQL_BRIDGE_BRIDGE_PLUGIN_H_
