#include "bridge_plugin.h"

#include <algorithm>
#include <cstdio>

#include "framework/core/plugin_registry.h"

namespace flowsql {
namespace bridge {

int BridgePlugin::Option(const char* arg) {
    // arg 格式: "python_path=/usr/bin/python3;port=18900;operators_dir=/path/to/operators"
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

        if (key == "python_path") python_path_ = val;
        else if (key == "host") host_ = val;
        else if (key == "port") port_ = std::stoi(val);
        else if (key == "operators_dir") operators_dir_ = val;

        pos = (end < opts.size()) ? end + 1 : opts.size();
    }
    return 0;
}

int BridgePlugin::Load() {
    // 在 Load 阶段获取 PluginRegistry（Meyers' Singleton，此时已安全初始化）
    registry_ = PluginRegistry::Instance();
    if (!registry_) {
        printf("BridgePlugin::Load: PluginRegistry not available\n");
        return -1;
    }

    // 如果 operators_dir 未配置，使用默认路径
    if (operators_dir_.empty()) {
        operators_dir_ = "operators";
    }

    printf("BridgePlugin::Load: python=%s, host=%s, port=%d, operators_dir=%s\n",
           python_path_.c_str(), host_.c_str(), port_, operators_dir_.c_str());
    return 0;
}

int BridgePlugin::Unload() {
    return 0;
}

int BridgePlugin::Start() {
    // 设置消息处理器
    process_manager_.SetMessageHandler(this);

    // 启动 Python Worker 进程（阻塞等待就绪）
    if (process_manager_.Start(host_, port_, operators_dir_, python_path_) != 0) {
        printf("BridgePlugin::Start: Failed to start Python Worker\n");
        return -1;
    }

    // 此时 OnWorkerReady 已被调用，算子已注册
    printf("BridgePlugin::Start: %zu Python operators registered\n", registered_keys_.size());
    return 0;
}

int BridgePlugin::Stop() {
    // 注销所有动态注册的算子
    for (const auto& key : registered_keys_) {
        registry_->Unregister(IID_OPERATOR, key);
        printf("BridgePlugin::Stop: Unregistered [%s]\n", key.c_str());
    }
    registered_keys_.clear();

    // 停止 Python Worker
    process_manager_.Stop();
    return 0;
}

// IMessageHandler 实现
void BridgePlugin::OnWorkerReady(const std::vector<OperatorMeta>& operators) {
    // 为每个算子创建 PythonOperatorBridge 并动态注册
    for (const auto& meta : operators) {
        auto bridge = std::make_shared<PythonOperatorBridge>(meta, host_, port_);
        std::string key = meta.catelog + "." + meta.name;
        registry_->Register(IID_OPERATOR, key, bridge);
        registered_keys_.push_back(key);
        printf("BridgePlugin: Registered operator [%s]\n", key.c_str());
    }
}

void BridgePlugin::OnOperatorAdded(const OperatorMeta& meta) {
    printf("BridgePlugin::OnOperatorAdded: %s.%s\n", meta.catelog.c_str(), meta.name.c_str());

    // 动态注册新算子
    auto bridge = std::make_shared<PythonOperatorBridge>(meta, host_, port_);
    std::string key = meta.catelog + "." + meta.name;
    registry_->Register(IID_OPERATOR, key, bridge);
    registered_keys_.push_back(key);
}

void BridgePlugin::OnOperatorRemoved(const std::string& catelog, const std::string& name) {
    printf("BridgePlugin::OnOperatorRemoved: %s.%s\n", catelog.c_str(), name.c_str());

    // 注销算子
    std::string key = catelog + "." + name;
    registry_->Unregister(IID_OPERATOR, key);

    // 从列表中移除
    auto it = std::find(registered_keys_.begin(), registered_keys_.end(), key);
    if (it != registered_keys_.end()) {
        registered_keys_.erase(it);
    }
}

void BridgePlugin::OnHeartbeat(const std::string& stats_json) {
    // 暂时不处理心跳
}

void BridgePlugin::OnError(int code, const std::string& message) {
    printf("BridgePlugin::OnError: code=%d, message=%s\n", code, message.c_str());
}

}  // namespace bridge
}  // namespace flowsql
