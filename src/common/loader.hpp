/*
 * Copyright (C) 2020-06 - FAST
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 *
 * Author       : fei.wang@foxmail.com
 * Date         : 2020-10-07 16:42:22
 * LastEditors  : kun.dong
 * LastEditTime : 2021-01-22 14:30:01
 */
#ifndef SRC_0_2_FAST_LOADER_HPP_
#define SRC_0_2_FAST_LOADER_HPP_

#include <functional>
#include <map>
#include <vector>
#include "toolkit.hpp"
#include "guid.h"
#include "typedef.h"
#include "iquerier.hpp"

namespace flowsql {
interface IRegister {
    virtual void Regist(const Guid &iid, void *iface) = 0;  // interface
};

// 86dc3d8-e65f-9a83-1a39-66d26e95a9ca
const Guid IID_PLUGIN = {0x86dc3d8, 0xe65f, 0x9a83, {0x1a, 0x39, 0x66, 0xd2, 0x6e, 0x95, 0xa9, 0xca}};
interface IPlugin {
    virtual ~IPlugin(){};

    virtual int Option(const char * /* arg */) { return 0; }
    virtual int Load() = 0;    // do not call any interface in this func.
    virtual int Unload() = 0;  // do not call any interface in this func.
};

// {5F6E092B-7FDE-46E4-A628-30115EEE11FF}
const Guid IID_MODULE = {0x5f6e092b, 0x7fde, 0x46e4, {0xa6, 0x28, 0x30, 0x11, 0x5e, 0xee, 0x11, 0xff}};
interface IModule {
    virtual ~IModule(){};
    virtual int Start() = 0;
    virtual int Stop() = 0;
};

class PluginLoader : public IRegister, public IQuerier {
 public:
    int Load(const char *fullpath[], int count);
    int Load(const char *path, const char *relapath[], const char *option[], int count);
    int Unload();

 public:
    // IRegister.
    virtual void Regist(const Guid &iid, void *iface);

    // IQuerier.
    virtual int Traverse(const Guid &iid, fntraverse proc);
    virtual void *First(const Guid &iid);

 public:
    static PluginLoader *Single() {
        static PluginLoader _loader;
        return &_loader;
    }

 private:
    PluginLoader() {}

 private:
    std::map<Guid, std::vector<void *>> ifs_ref_;
    std::vector<thandle> plugins_ref_;
};

typedef flowsql::IPlugin *(*fnregister)(flowsql::IRegister *, const char *);
inline int PluginLoader::Load(const char *fullpath[], int count) {
    std::string app_path = get_absolute_process_path();
    app_path += "/";
    for (int pos = 0; pos < count; ++pos) {
        std::string lib_path = app_path + fullpath[pos];
        thandle h = loadlibrary(lib_path.c_str());
        if (!h) {
            printf("Load shared library '%s' faild, error : '%s'\n", lib_path.c_str(), getlasterror());
            return -1;
        }

        // iface regist.
        fnregister fregist = (fnregister)getprocaddress(h, "pluginregist");
        if (!fregist) {
            printf("'%s' getprocaddress of '%s' faild\n", lib_path.c_str(), "pluginregist");
            freelibrary(h);
            return -1;
        }
        flowsql::IPlugin *plugin = fregist(this, nullptr);
        if (-1 == plugin->Load()) {
            printf("'%s' plugin load faild\n", lib_path.c_str());
            freelibrary(h);
            return -1;
        }

        // do iplugin load.
        plugins_ref_.push_back(h);
    }
    // // traverse all iplugin iface call load()
    // this->Traverse(flowsql::IID_PLUGIN, [](void *imod) {
    //     flowsql::IPlugin *iplugin_ = reinterpret_cast<flowsql::IPlugin *>(imod);
    //     return iplugin_->Load();
    // });
    return 0;
}

inline int PluginLoader::Load(const char *path, const char *relapath[], const char *option[], int count) {
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

        // iface regist.
        fnregister fregist = (fnregister)getprocaddress(h, "pluginregist");
        if (!fregist) {
            printf("'%s' getprocaddress of '%s' faild\n", relapath[pos], "pluginregist");
            freelibrary(h);
            return -1;
        }

        flowsql::IPlugin *plugin = fregist(this, option[pos]);
        if (-1 == plugin->Load()) {
            printf("'%s' plugin load faild\n", relapath[pos]);
            freelibrary(h);
            return -1;
        }

        // do iplugin load.
        plugins_ref_.push_back(h);
    }
    // // traverse all iplugin iface call load()
    // this->Traverse(flowsql::IID_PLUGIN, [](void *imod) {
    //     flowsql::IPlugin *iplugin_ = reinterpret_cast<flowsql::IPlugin *>(imod);
    //     return iplugin_->Load();
    // });
    return 0;
}

typedef void (*fnunregister)();
inline int PluginLoader::Unload() {
    this->Traverse(flowsql::IID_PLUGIN, [](void *imod) {
        flowsql::IPlugin *iplugin_ = reinterpret_cast<flowsql::IPlugin *>(imod);
        return iplugin_->Unload();
    });

    for (thandle h : plugins_ref_) {
        fnunregister funregist = (fnunregister)getprocaddress(h, "pluginunregist");
        if (funregist) {
            funregist();
        }
        freelibrary(h);
    }
    return 0;
}

inline void PluginLoader::Regist(const Guid &iid, void *iface) {
    // insert iface.
    ifs_ref_[iid].push_back(iface);
}

inline int PluginLoader::Traverse(const Guid &iid, fntraverse proc) {
    auto _i = ifs_ref_.find(iid);
    if (_i != ifs_ref_.end()) {
        for (auto &i : _i->second) {
            if (-1 == proc(i)) {
                break;
            }
        }
    }
    return 0;
}

inline void *PluginLoader::First(const Guid &iid) {
    auto _i = ifs_ref_.find(iid);
    if (_i != ifs_ref_.end() && !_i->second.empty()) {
        return _i->second[0];
    }
    return nullptr;
}

}  // namespace flowsql

EXPORT_API flowsql::IQuerier *getiquerier() {
    flowsql::PluginLoader *_loader = flowsql::PluginLoader::Single();
    return dynamic_cast<flowsql::IQuerier *>(_loader);
}

#define BEGIN_PLUGIN_REGIST(classname)                                                   \
    EXPORT_API void pluginunregist() {}                                                  \
                                                                                         \
    EXPORT_API flowsql::IPlugin *pluginregist(flowsql::IRegister *registry, const char *opt) { \
        static classname _plugin;
// const char *mname = #classname;

#define ____INTERFACE(iid, intername)                           \
    {                                                           \
        intername *iface = dynamic_cast<intername *>(&_plugin); \
        registry->Regist(iid, iface);                           \
    }

#define END_PLUGIN_REGIST() \
    _plugin.Option(opt);    \
    return &_plugin;        \
    }

#endif  // SRC_0_2_FAST_LOADER_HPP_
