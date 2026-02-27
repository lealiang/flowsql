#ifndef _FLOWSQL_WEB_WEB_SERVER_H_
#define _FLOWSQL_WEB_WEB_SERVER_H_

#include <httplib.h>

#include <memory>
#include <string>

#include "db/database.h"

namespace flowsql {

class PluginRegistry;

namespace web {

// Web 管理系统主服务器
class WebServer {
 public:
    WebServer();
    ~WebServer() = default;

    // 初始化：打开数据库、注册路由、同步插件信息
    int Init(const std::string& db_path, PluginRegistry* registry);

    // 启动监听（阻塞）
    int Start(const std::string& host, int port);

    // 停止
    void Stop();

    Database& GetDatabase() { return db_; }
    PluginRegistry* GetRegistry() { return registry_; }

 private:
    // 注册所有 API 路由
    void RegisterRoutes();

    // 同步 PluginRegistry 中的通道和算子信息到 SQLite
    void SyncRegistryToDb();

    // API 处理函数
    void HandleHealth(const httplib::Request& req, httplib::Response& res);
    void HandleGetChannels(const httplib::Request& req, httplib::Response& res);
    void HandleGetOperators(const httplib::Request& req, httplib::Response& res);
    void HandleUploadOperator(const httplib::Request& req, httplib::Response& res);
    void HandleActivateOperator(const httplib::Request& req, httplib::Response& res);
    void HandleDeactivateOperator(const httplib::Request& req, httplib::Response& res);
    void HandleGetTasks(const httplib::Request& req, httplib::Response& res);
    void HandleCreateTask(const httplib::Request& req, httplib::Response& res);
    void HandleGetTaskResult(const httplib::Request& req, httplib::Response& res);

    httplib::Server server_;
    Database db_;
    PluginRegistry* registry_ = nullptr;
};

}  // namespace web
}  // namespace flowsql

#endif  // _FLOWSQL_WEB_WEB_SERVER_H_
