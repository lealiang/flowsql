#include "plugin_registry.h"

#include <cstdio>

namespace flowsql {

PluginRegistry* PluginRegistry::Instance() {
    static PluginRegistry instance;
    return &instance;
}

PluginRegistry::PluginRegistry() : loader_(PluginLoader::Single()) {}

PluginRegistry::~PluginRegistry() {}

int PluginRegistry::LoadPlugin(const std::string& path) {
    const char* paths[] = {path.c_str()};
    int ret = loader_->Load(paths, 1);
    if (ret == 0) {
        index_built_ = false;
    }
    return ret;
}

void PluginRegistry::UnloadAll() {
    loader_->Unload();
    static_index_.clear();
    dynamic_index_.clear();
    index_built_ = false;
}

void PluginRegistry::Register(const Guid& iid, const std::string& key, std::shared_ptr<void> instance) {
    dynamic_index_[iid][key] = std::move(instance);
}

void PluginRegistry::Unregister(const Guid& iid, const std::string& key) {
    auto it = dynamic_index_.find(iid);
    if (it != dynamic_index_.end()) {
        it->second.erase(key);
        if (it->second.empty()) {
            dynamic_index_.erase(it);
        }
    }
}

void PluginRegistry::BuildIndex() {
    if (index_built_) return;
    static_index_.clear();

    // Channel 索引
    if (auto* channels = loader_->GetInterfaces(IID_CHANNEL)) {
        for (void* p : *channels) {
            auto* ch = static_cast<IChannel*>(p);
            static_index_[IID_CHANNEL][ch->Catelog() + "." + ch->Name()] = p;
        }
    }

    // Operator 索引
    if (auto* operators = loader_->GetInterfaces(IID_OPERATOR)) {
        for (void* p : *operators) {
            auto* op = static_cast<IOperator*>(p);
            static_index_[IID_OPERATOR][op->Catelog() + "." + op->Name()] = p;
        }
    }

    index_built_ = true;
}

void* PluginRegistry::Get(const Guid& iid, const std::string& key) {
    BuildIndex();

    // 动态优先
    auto dit = dynamic_index_.find(iid);
    if (dit != dynamic_index_.end()) {
        auto it = dit->second.find(key);
        if (it != dit->second.end()) return it->second.get();
    }

    // 静态
    auto sit = static_index_.find(iid);
    if (sit != static_index_.end()) {
        auto it = sit->second.find(key);
        if (it != sit->second.end()) return it->second;
    }
    return nullptr;
}

void PluginRegistry::Traverse(const Guid& iid, std::function<int(void*)> callback) {
    BuildIndex();

    // 先遍历动态
    auto dit = dynamic_index_.find(iid);
    if (dit != dynamic_index_.end()) {
        for (auto& [key, ptr] : dit->second) {
            if (callback(ptr.get()) == -1) return;
        }
    }

    // 再遍历静态（跳过已被动态覆盖的 key）
    auto sit = static_index_.find(iid);
    if (sit != static_index_.end()) {
        for (auto& [key, ptr] : sit->second) {
            if (dit != dynamic_index_.end() && dit->second.count(key)) continue;
            if (callback(ptr) == -1) return;
        }
    }
}

}  // namespace flowsql
