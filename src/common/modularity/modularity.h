/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2020-06-29 14:41:37
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
#ifndef _FLOWSQL_COMMON_MODULARITY_MODULARITY_H_
#define _FLOWSQL_COMMON_MODULARITY_MODULARITY_H_

#include <fast/typedef.h>
#include <string.h>
#include <fast/loader.hpp>
#include <list>
#include <map>
#include <string>
#include <vector>
#include "../threadsafe/lock.h"
#include "mbasic.h"

#define MAX_ERROR_INFO_LEN 1024

typedef const char *(*fnregister)(fast::IRegister *, const char *);

namespace flowsql {
class InterfaceRegister : public IRegister, public IQuerier {
 public:
    InterfaceRegister() {}

    ~InterfaceRegister() {}
    virtual int32_t Register(std::shared_ptr<IModule> module) {
        const char *mname = module->Name();
        fast::wlocker wlock(lock_);
        if (modules_.find(mname) == modules_.end()) {
            modules_.insert(std::make_pair(mname, module));
            return 0;
        } else {
            printf("Module '%s' already loaded.\n", mname);
            return -1;
        }
    }

    virtual void Unregister(const char *mname) {
        fast::wlocker wlock(lock_);
        modules_.erase(mname);
        ifs_life_.erase(mname);

        // erase weak_ptr
        for (auto &mis : ifs_ref_) {
            for (auto _j = mis.second.begin(); _j != mis.second.end(); ++_j) {
                if (_j->ifce.expired()) {
                    _j = mis.second.erase(_j);
                }
            }
        }
    }

    virtual void Register(const char *mname, Guid iid, void *i) {
        fast::wlocker wlock(lock_);

        // interface life cycle map
        auto module = get_module(mname);
        if (module) {
            std::shared_ptr<void> ilife(std::shared_ptr<void>(std::static_pointer_cast<void>(module), i));
            std::weak_ptr<void> iref(ilife);
            ifs_life_[mname].push_back(ilife);
            ifs_ref_[iid].push_back(_nameifce{mname, iref});
        }
    }

    virtual std::shared_ptr<IModule> Query(const char *mname) {
        fast::rlocker rlock(lock_);
        return get_module(mname);
    }

    // Return the first interface iid
    virtual std::weak_ptr<void> Query(Guid iid) {
        fast::rlocker rlock(lock_);
        return get_first_interface(iid);
    }

    virtual int32_t Traverse(Guid iid, std::function<int32_t(const char *mname, std::weak_ptr<void> intr)> proc) {
        fast::rlocker rlock(lock_);
        auto _i = ifs_ref_.find(iid);
        if (_i != ifs_ref_.end()) {
            for (auto &i : _i->second) {
                if (-1 == proc(i.name, i.ifce)) {
                    break;
                }
            }
        }

        return 0;
    }

    virtual int32_t Traverse(std::function<int32_t(const char *, std::shared_ptr<IModule>)> proc) {
        fast::rlocker rlock(lock_);
        for (auto &module : modules_) {
            if (-1 == proc(module.first.c_str(), module.second)) {
                return -1;
            }
        }

        return 0;
    }

 protected:
    std::shared_ptr<IModule> get_module(const char *mname) {
        auto _i = modules_.find(mname);
        if (_i != modules_.end()) {
            return _i->second;
        } else {
            return nullptr;
        }
    }

    std::weak_ptr<void> get_first_interface(Guid iid) {
        auto _i = ifs_ref_.find(iid);
        if (_i != ifs_ref_.end()) {
            for (auto &i : _i->second) {
                return i.ifce;
            }
        }
        return std::weak_ptr<void>();
    }

 protected:
    struct _nameifce {
        const char *name;
        std::weak_ptr<void> ifce;
    };
    std::map<std::string, std::shared_ptr<IModule>> modules_;
    std::map<std::string, std::vector<std::shared_ptr<void>>> ifs_life_;
    std::map<Guid, std::list<_nameifce>> ifs_ref_;
    fast::rwlock lock_;
};

class MDriver {
 public:
    MDriver() {}
    ~MDriver() {}

 public:
    int32_t Load(const char *path, const char *names[], const char *options[], int32_t count) {
        char realpath[PATH_MAX] = {0};
        size_t pathlen = strlen(path);
        if (pathlen + 1 >= PATH_MAX) {
            printf("library path '%s' too long.\n", path);
            return -1;
        }
        memcpy(realpath, path, pathlen);
        realpath[pathlen++] = '/';
        for (int32_t pos = 0; pos < count; ++pos) {
            if (names[pos][0] == '/' || names[pos][0] == '.') {
                load_library("", names[pos], options[pos]);
            } else {
                load_library(realpath, names[pos], options[pos]);
            }
        }

        return 0;
    }

    int32_t Load(const char *path, const char *name, const char *option) {
        char realpath[PATH_MAX] = {0};
        size_t pathlen = strlen(path);
        if (pathlen + 1 >= PATH_MAX) {
            printf("library path '%s' too long.\n", path);
            return -1;
        }
        memcpy(realpath, path, pathlen);
        realpath[pathlen++] = '/';
        if (name[0] == '/' || name[0] == '.') {
            return load_library("", name, option);
        } else {
            return load_library(realpath, name, option);
        }
    }

    void Unload(const char *name) {
        auto _i = handles_.find(name);
        if (_i != handles_.end()) {
            auto module = iregister_.Query(_i->second.first.c_str());
            module->Unload();
            iregister_.Unregister(_i->second.first.c_str());

            FreeLibrary(_i->second.second);
            handles_.erase(_i);
        }
    }

    int32_t Run() {
        return iregister_.Traverse([this](const char *mname, std::shared_ptr<IModule> m) -> int32_t {
            m->Querier(&this->iregister_);
            return m->Load();
        });
    }

    void Stop() {
        iregister_.Traverse([](const char *mname, std::shared_ptr<IModule> m) -> int32_t {
            m->Unload();
            return 0;
        });
    }

    void Release() {
        for (auto &hdl : handles_) {
            FreeLibrary(hdl.second.second);
        }
    }

 protected:
    int32_t load_library(const char *path, const char *name, const char *option) {
        std::string pathname = path;
        pathname.append(name);
        tdLibHandle h = LoadShareLibrary(pathname.c_str());
        if (!h) {
            // todo set last error
#if defined(__linux__) || defined(__APPLE__)
            printf("Load shared library '%s' faild, error : '%s'\n", name, dlerror());
#else
            printf("Load shared library '%s' faild, error : '%s'\r\n", name, GetLastError());
#endif
            return -1;
        }

        fnregister fregist = (fnregister)GetProcAddress(h, "RegisterInterface");
        if (!fregist) {
            // todo set last error
            printf("'%s' GetProcAddress of '%s' faild\n", name, "RegisterInterface");
            return -1;
        }
        const char *mname = fregist(&iregister_, option);
        if (mname) {
            handles_[name] = std::make_pair(mname, h);
            return 0;
        }
        FreeLibrary(h);
        return -1;
    }

 protected:
    InterfaceRegister iregister_;
    std::map<std::string, std::pair<std::string, tdLibHandle>> handles_;
};
}  // namespace flowsql

#endif  // _FLOWSQL_COMMON_MODULARITY_MODULARITY_H_
