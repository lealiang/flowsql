#include "bridge_plugin.h"

#include <rapidjson/document.h>

#include <cstdio>
#include <cstring>

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
        else if (key == "ready_timeout") ready_timeout_ms_ = std::stoi(val);

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
    // 启动 Python Worker 进程
    if (process_manager_.Start(host_, port_, operators_dir_, python_path_) != 0) {
        printf("BridgePlugin::Start: Failed to start Python Worker\n");
        return -1;
    }

    // 等待 Worker 就绪
    if (process_manager_.WaitReady(ready_timeout_ms_) != 0) {
        printf("BridgePlugin::Start: Python Worker not ready\n");
        process_manager_.Stop();
        return -1;
    }

    // 查询 Python 算子列表
    auto operators = QueryPythonOperators();
    if (operators.empty()) {
        printf("BridgePlugin::Start: No Python operators found (Worker running but no operators)\n");
        // 不算失败，Worker 可能后续添加算子
    }

    // 为每个算子创建 PythonOperatorBridge 并动态注册
    for (const auto& meta : operators) {
        auto bridge = std::make_shared<PythonOperatorBridge>(meta, host_, port_);
        std::string key = meta.catelog + "." + meta.name;
        registry_->Register(IID_OPERATOR, key, bridge);
        registered_keys_.push_back(key);
        printf("BridgePlugin::Start: Registered Python operator [%s]\n", key.c_str());
    }

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

std::vector<OperatorMeta> BridgePlugin::QueryPythonOperators() {
    std::vector<OperatorMeta> result;

    httplib::Client client(host_, port_);
    client.set_connection_timeout(5);
    client.set_read_timeout(5);

    auto res = client.Get("/operators");
    if (!res || res->status != 200) {
        printf("BridgePlugin: GET /operators failed\n");
        return result;
    }

    // 解析 JSON 响应
    rapidjson::Document doc;
    doc.Parse(res->body.c_str());
    if (doc.HasParseError() || !doc.IsArray()) {
        printf("BridgePlugin: Invalid /operators response\n");
        return result;
    }

    for (auto& item : doc.GetArray()) {
        if (!item.IsObject()) continue;

        OperatorMeta meta;
        if (item.HasMember("catelog") && item["catelog"].IsString())
            meta.catelog = item["catelog"].GetString();
        if (item.HasMember("name") && item["name"].IsString())
            meta.name = item["name"].GetString();
        if (item.HasMember("description") && item["description"].IsString())
            meta.description = item["description"].GetString();
        if (item.HasMember("position") && item["position"].IsString()) {
            std::string pos = item["position"].GetString();
            meta.position = (pos == "STORAGE") ? OperatorPosition::STORAGE : OperatorPosition::DATA;
        }

        if (!meta.catelog.empty() && !meta.name.empty()) {
            result.push_back(std::move(meta));
        }
    }

    return result;
}

int BridgePlugin::Restart() {
    // 完整重启流程：Stop → Start
    printf("BridgePlugin::Restart: Performing full restart\n");

    // 注销旧注册
    for (const auto& key : registered_keys_) {
        registry_->Unregister(IID_OPERATOR, key);
    }
    registered_keys_.clear();

    // 停止旧进程
    process_manager_.Stop();

    // 重新启动
    return Start();
}

}  // namespace bridge
}  // namespace flowsql
