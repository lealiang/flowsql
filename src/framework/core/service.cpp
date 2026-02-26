#include "service.h"

#include <cstdio>

#include <common/toolkit.hpp>

namespace flowsql {

int Service::Init(int argc, char* argv[]) {
    registry_ = PluginRegistry::Instance();
    return 0;
}

int Service::LoadPlugins(const std::string& config_path) {
    enum_files(config_path.c_str(), [this, &config_path](const char*, struct dirent* entry) {
        std::string name(entry->d_name);
        if (name.size() > 3 && name.substr(name.size() - 3) == ".so") {
            std::string full_path = config_path + "/" + name;
            int ret = registry_->LoadPlugin(full_path);
            if (ret != 0) {
                printf("Failed to load plugin: %s\n", full_path.c_str());
            }
        }
    });
    return 0;
}

int Service::Run() {
    running_ = true;
    for (auto& pipeline : pipelines_) {
        pipeline->Run();
    }
    return 0;
}

void Service::Shutdown() {
    running_ = false;
    for (auto& pipeline : pipelines_) {
        pipeline->Stop();
    }
    registry_->UnloadAll();
}

}  // namespace flowsql
