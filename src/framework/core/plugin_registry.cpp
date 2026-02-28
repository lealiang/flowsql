#include "plugin_registry.h"

#include <vector>

namespace flowsql {

PluginRegistry* PluginRegistry::Instance() {
    static PluginRegistry instance;
    return &instance;
}

PluginRegistry::PluginRegistry() : loader_(PluginLoader::Single()) {}

PluginRegistry::~PluginRegistry() {}

int PluginRegistry::LoadPlugin(const std::string& path) {
    {
        std::unique_lock lock(mutex_);
        const char* paths[] = {path.c_str()};
        int ret = loader_->Load(paths, 1);
        if (ret != 0) return ret;
        BuildIndex();
    }
    // 锁已释放，安全启动模块（模块的 Start() 可能回调 Register()）
    return loader_->StartModules();
}

void PluginRegistry::UnloadAll() {
    // 先在锁外停止模块（Stop 可能回调 Unregister，持锁会死锁）
    loader_->StopModules();

    std::unique_lock lock(mutex_);
    // 先析构动态对象（shared_ptr），再卸载 .so（问题 4）
    // 否则 .so 卸载后析构函数代码已被卸载，导致段错误
    dynamic_index_.clear();
    static_index_.clear();
    loader_->Unload();
}

void PluginRegistry::Register(const Guid& iid, const std::string& key, std::shared_ptr<void> instance) {
    std::unique_lock lock(mutex_);
    dynamic_index_[iid][key] = std::move(instance);
}

void PluginRegistry::Unregister(const Guid& iid, const std::string& key) {
    std::unique_lock lock(mutex_);
    auto it = dynamic_index_.find(iid);
    if (it != dynamic_index_.end()) {
        it->second.erase(key);
        if (it->second.empty()) {
            dynamic_index_.erase(it);
        }
    }
}

int PluginRegistry::StartModules() {
    // 不持有 mutex_，直接委托 PluginLoader
    // 模块的 Start() 可能回调 Register()，如果持锁会死锁
    return loader_->StartModules();
}

void PluginRegistry::StopModules() {
    loader_->StopModules();
}
// BuildIndex 内部方法，调用者需持有写锁
void PluginRegistry::BuildIndex() {
    static_index_.clear();

    // Channel 索引
    if (auto* channels = loader_->GetInterfaces(IID_CHANNEL)) {
        for (void* p : *channels) {
            auto* ch = static_cast<IChannel*>(p);
            static_index_[IID_CHANNEL][std::string(ch->Catelog()) + "." + ch->Name()] = p;
        }
    }

    // Operator 索引
    if (auto* operators = loader_->GetInterfaces(IID_OPERATOR)) {
        for (void* p : *operators) {
            auto* op = static_cast<IOperator*>(p);
            static_index_[IID_OPERATOR][op->Catelog() + "." + op->Name()] = p;
        }
    }
}

void* PluginRegistry::Get(const Guid& iid, const std::string& key) {
    std::shared_lock lock(mutex_);

    // 动态优先
    auto dit = dynamic_index_.find(iid);
    if (dit != dynamic_index_.end()) {
        auto it = dit->second.find(key);
        if (it != dit->second.end()) {
            return it->second.get();
        }
    }

    // 静态
    auto sit = static_index_.find(iid);
    if (sit != static_index_.end()) {
        auto it = sit->second.find(key);
        if (it != sit->second.end()) {
            return it->second;
        }
    }

    return nullptr;
}

void PluginRegistry::Traverse(const Guid& iid, std::function<int(void*)> callback) {
    // 在锁内复制指针列表，释放锁后再执行回调
    // 避免回调中调用 Register/Unregister 导致死锁
    // 动态部分复制 shared_ptr 延长生命周期，防止锁释放后裸指针悬空（问题 3）
    std::vector<std::shared_ptr<void>> dynamic_ptrs;
    std::vector<void*> static_ptrs;
    {
        std::shared_lock lock(mutex_);

        // 先收集动态（持有 shared_ptr 副本）
        std::unordered_map<std::string, bool> seen;
        auto dit = dynamic_index_.find(iid);
        if (dit != dynamic_index_.end()) {
            for (auto& [key, ptr] : dit->second) {
                dynamic_ptrs.push_back(ptr);
                seen[key] = true;
            }
        }

        // 再收集静态（跳过已被动态覆盖的 key）
        auto sit = static_index_.find(iid);
        if (sit != static_index_.end()) {
            for (auto& [key, ptr] : sit->second) {
                if (seen.count(key)) continue;
                static_ptrs.push_back(ptr);
            }
        }
    }

    // 锁已释放，安全执行回调
    for (auto& sp : dynamic_ptrs) {
        if (callback(sp.get()) == -1) return;
    }
    for (void* p : static_ptrs) {
        if (callback(p) == -1) return;
    }
}

}  // namespace flowsql
