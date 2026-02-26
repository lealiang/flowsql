#ifndef _FLOWSQL_FRAMEWORK_CORE_SERVICE_H_
#define _FLOWSQL_FRAMEWORK_CORE_SERVICE_H_

#include <memory>
#include <string>
#include <vector>

#include "pipeline.h"
#include "plugin_registry.h"

namespace flowsql {

class Service {
 public:
    Service() = default;
    ~Service() = default;

    int Init(int argc, char* argv[]);
    int LoadPlugins(const std::string& config_path);
    int Run();
    void Shutdown();

    PluginRegistry* Registry() { return registry_; }

 private:
    PluginRegistry* registry_;
    std::vector<std::unique_ptr<Pipeline>> pipelines_;
    bool running_ = false;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_SERVICE_H_
