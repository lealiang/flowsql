#ifndef _FLOWSQL_SERVICES_DATABASE_DATABASE_PLUGIN_H_
#define _FLOWSQL_SERVICES_DATABASE_DATABASE_PLUGIN_H_

#include <common/iplugin.h>
#include <framework/interfaces/idatabase_factory.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "database_channel.h"

namespace flowsql {
namespace database {

// DatabasePlugin — 数据库通道工厂插件
// 同时实现 IPlugin（生命周期）和 IDatabaseFactory（通道工厂）
class __attribute__((visibility("default"))) DatabasePlugin : public IPlugin, public IDatabaseFactory {
 public:
    DatabasePlugin() = default;
    ~DatabasePlugin() override = default;

    // IPlugin
    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;

    // IDatabaseFactory
    IDatabaseChannel* Get(const char* type, const char* name) override;
    void List(std::function<void(const char* type, const char* name,
                                  const char* config_json)> callback) override;
    int Release(const char* type, const char* name) override;
    const char* LastError() override;

    // IDatabaseFactory 动态管理（Epic 6）
    int AddChannel(const char* config_str) override;
    int RemoveChannel(const char* type, const char* name) override;
    int UpdateChannel(const char* config_str) override;

 private:
    // 创建指定类型的数据库驱动
    std::unique_ptr<IDbDriver> CreateDriver(const std::string& type);

    // 解析单个数据库配置（不加锁，调用方负责加锁）
    int ParseSingleConfig(const char* arg);

    // YAML 持久化
    int LoadFromYaml();   // Start() 时调用，加载 flowsql.yml
    int SaveToYaml();     // Add/Remove/Update 后调用，写回 flowsql.yml

    // 密码 AES-256-GCM 加解密
    std::string EncryptPassword(const std::string& plain);
    std::string DecryptPassword(const std::string& cipher);

    // 通道池：key = "type.name"
    std::unordered_map<std::string, std::shared_ptr<DatabaseChannel>> channels_;

    // 驱动存储：key = "type.name"（持有所有权）
    std::unordered_map<std::string, std::unique_ptr<IDbDriver>> driver_storage_;

    // 配置表：key = "type.name", value = 连接参数（明文）
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> configs_;

    std::mutex mutex_;
    std::mutex save_mutex_;  // 保证 SaveToYaml 串行写入，防止并发写覆盖
    IQuerier* querier_ = nullptr;

    // flowsql.yml 路径，从 Option("config_file=...") 解析
    std::string config_file_;

    // 线程安全的错误信息存储
    static thread_local std::string last_error_;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_DATABASE_PLUGIN_H_
