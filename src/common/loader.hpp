/*
 * Copyright (C) 2020-06 - flowSQL
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 * PluginLoader — 插件加载器实现
 * 只有主程序需要 include 此文件，插件只需 include iplugin.h
 */
#ifndef _FLOWSQL_COMMON_LOADER_HPP_
#define _FLOWSQL_COMMON_LOADER_HPP_

#include <functional>
#include <map>
#include <vector>
#include "toolkit.hpp"
#include "iplugin.h"

namespace flowsql {

class PluginLoader : public IRegister, public IQuerier {
 public:
    int Load(const char* fullpath[], int count);
    int Load(const char* path, const char* relapath[], const char* option[], int count);
    int Unload();

 public:
    // IRegister
    virtual void Regist(const Guid& iid, void* iface);

    // IQuerier
    virtual int Traverse(const Guid& iid, fntraverse proc);
    virtual void* First(const Guid& iid);

 public:
    static PluginLoader* Single() {
        static PluginLoader _loader;
        return &_loader;
    }

 public:
    // 插件启停
    int StartAll();
    void StopAll();

 private:
    PluginLoader() {}

    std::map<Guid, std::vector<void*>> ifs_ref_;
    std::vector<thandle> plugins_ref_;
    size_t started_count_ = 0;
};

typedef flowsql::IPlugin* (*fnregister)(flowsql::IRegister*, const char*);
inline int PluginLoader::Load(const char* fullpath[], int count) {
    std::string app_path = get_absolute_process_path();
    app_path += "/";
    for (int pos = 0; pos < count; ++pos) {
        std::string lib_path = app_path + fullpath[pos];
        thandle h = loadlibrary(lib_path.c_str());
        if (!h) {
            printf("Load shared library '%s' faild, error : '%s'\n", lib_path.c_str(), getlasterror());
            return -1;
        }

        fnregister fregist = (fnregister)getprocaddress(h, "pluginregist");
        if (!fregist) {
            printf("'%s' getprocaddress of '%s' faild\n", lib_path.c_str(), "pluginregist");
            freelibrary(h);
            return -1;
        }

        size_t prev_count = ifs_ref_[flowsql::IID_PLUGIN].size();
        fregist(this, nullptr);

        // 遍历本次 .so 注册的所有 IPlugin 调用 Load(IQuerier*)
        auto& plugins = ifs_ref_[flowsql::IID_PLUGIN];
        for (size_t i = prev_count; i < plugins.size(); ++i) {
            auto* plugin = reinterpret_cast<flowsql::IPlugin*>(plugins[i]);
            if (-1 == plugin->Load(this)) {
                printf("'%s' plugin load faild\n", lib_path.c_str());
                freelibrary(h);
                return -1;
            }
        }

        plugins_ref_.push_back(h);
    }
    return 0;
}

inline int PluginLoader::Load(const char* path, const char* relapath[], const char* option[], int count) {
    char realpath[PATH_MAX] = {0};
    size_t pathlen = strlen(path);
    if (pathlen + 1 >= PATH_MAX) {
        printf("library path '%s' too long.\n", path);
        return -1;
    }
    strncpy(realpath, path, pathlen);
    realpath[pathlen++] = '/';
    for (int pos = 0; pos < count; ++pos) {
        thandle h = 0;
        if (relapath[pos][0] == '/' || relapath[pos][0] == '.') {
            h = loadlibrary(relapath[pos]);
        } else {
            strncpy(realpath + pathlen, relapath[pos], strlen(relapath[pos]) + 1);
            h = loadlibrary(realpath);
        }
        if (!h) {
            printf("Load shared library '%s' faild, error : '%s'\n", relapath[pos], getlasterror());
            return -1;
        }

        fnregister fregist = (fnregister)getprocaddress(h, "pluginregist");
        if (!fregist) {
            printf("'%s' getprocaddress of '%s' faild\n", relapath[pos], "pluginregist");
            freelibrary(h);
            return -1;
        }

        size_t prev_count = ifs_ref_[flowsql::IID_PLUGIN].size();
        fregist(this, option[pos]);

        // 遍历本次 .so 注册的所有 IPlugin 调用 Load(IQuerier*)
        auto& plugins = ifs_ref_[flowsql::IID_PLUGIN];
        for (size_t i = prev_count; i < plugins.size(); ++i) {
            auto* plugin = reinterpret_cast<flowsql::IPlugin*>(plugins[i]);
            if (-1 == plugin->Load(this)) {
                printf("'%s' plugin load faild\n", relapath[pos]);
                freelibrary(h);
                return -1;
            }
        }

        plugins_ref_.push_back(h);
    }
    return 0;
}

typedef void (*fnunregister)();
inline int PluginLoader::Unload() {
    this->Traverse(flowsql::IID_PLUGIN, [](void* imod) {
        flowsql::IPlugin* iplugin_ = reinterpret_cast<flowsql::IPlugin*>(imod);
        return iplugin_->Unload();
    });

    for (thandle h : plugins_ref_) {
        fnunregister funregist = (fnunregister)getprocaddress(h, "pluginunregist");
        if (funregist) {
            funregist();
        }
        freelibrary(h);
    }

    plugins_ref_.clear();
    ifs_ref_.clear();
    started_count_ = 0;

    return 0;
}

inline void PluginLoader::Regist(const Guid& iid, void* iface) {
    ifs_ref_[iid].push_back(iface);
}

inline int PluginLoader::Traverse(const Guid& iid, fntraverse proc) {
    auto _i = ifs_ref_.find(iid);
    if (_i != ifs_ref_.end()) {
        for (auto& i : _i->second) {
            if (-1 == proc(i)) {
                break;
            }
        }
    }
    return 0;
}

inline void* PluginLoader::First(const Guid& iid) {
    auto _i = ifs_ref_.find(iid);
    if (_i != ifs_ref_.end() && !_i->second.empty()) {
        return _i->second[0];
    }
    return nullptr;
}

inline int PluginLoader::StartAll() {
    auto it = ifs_ref_.find(flowsql::IID_PLUGIN);
    if (it == ifs_ref_.end()) return 0;

    auto& plugins = it->second;
    for (size_t i = started_count_; i < plugins.size(); ++i) {
        auto* plugin = reinterpret_cast<flowsql::IPlugin*>(plugins[i]);
        if (-1 == plugin->Start()) {
            printf("IPlugin::Start() failed at index %zu, rolling back\n", i);
            for (size_t j = i; j > started_count_; --j) {
                auto* started = reinterpret_cast<flowsql::IPlugin*>(plugins[j - 1]);
                started->Stop();
            }
            return -1;
        }
    }
    started_count_ = plugins.size();
    return 0;
}

inline void PluginLoader::StopAll() {
    auto it = ifs_ref_.find(flowsql::IID_PLUGIN);
    if (it == ifs_ref_.end()) return;

    auto& plugins = it->second;
    for (size_t i = started_count_; i > 0; --i) {
        auto* plugin = reinterpret_cast<flowsql::IPlugin*>(plugins[i - 1]);
        plugin->Stop();
    }
    started_count_ = 0;
}

}  // namespace flowsql

#endif  // _FLOWSQL_COMMON_LOADER_HPP_
