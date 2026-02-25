/*
 * Copyright (C) 2020-06 - FATEAM
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 *
 * Author       : lealiang@qq.com
 * Date         : 2020-06-29 11:09:05
 * LastEditors  : kun.dong
 * LastEditTime : 2020-10-22 10:59:38
 */
#ifndef _FAST_COMMON_MODULARITY_MBASIC_H_
#define _FAST_COMMON_MODULARITY_MBASIC_H_

#include <fast/typedef.h>
#include <functional>
#include <memory>
#include "../guid.h"

namespace flowsql {
// client interface
class IModule;
interface IRegister {
    virtual int32_t Register(std::shared_ptr<IModule> module) = 0;    // module interface
    virtual void Register(const char *mname, Guid iid, void *i) = 0;  // interface
};

interface IQuerier {
    virtual std::shared_ptr<IModule> Query(const char *mname) = 0;  // module interface
    virtual std::weak_ptr<void> Query(Guid iid) = 0;                // interface
    virtual int32_t Traverse(Guid iid, std::function<int32_t(const char *, std::weak_ptr<void>)> proc) = 0;
    virtual int32_t Traverse(std::function<int32_t(const char *, std::shared_ptr<IModule>)> proc) = 0;
};

// {5F6E092B-7FDE-46E4-A628-30115EEE11FF}
const Guid IID_MODULE = {0x5f6e092b, 0x7fde, 0x46e4, {0xa6, 0x28, 0x30, 0x11, 0x5e, 0xee, 0x11, 0xff}};
class IModule {
 public:
    IModule() : querier_(nullptr) {}
    virtual ~IModule() {}

    virtual Guid IID() const { return IID_MODULE; }
    virtual const char *Name() const { return "fast::IModule"; };

    // overwrite by derive class
    virtual int32_t Option(const char *) { return 0; }
    virtual int32_t Load() = 0;
    virtual void Unload() = 0;

    void Querier(IQuerier *querier) { querier_ = querier; }

 protected:
    template <typename InterType>
    std::weak_ptr<InterType> QueryInterface(Guid iid) {
        return std::weak_ptr<InterType>(querier_->Query(iid));
    }

    template <typename InterType>
    int32_t Traverse(Guid iid, std::function<int32_t(const char *mname, InterType *intr)> proc) {
        querier_->Traverse(iid, [&](const char *mname, std::weak_ptr<void> intr) -> int32_t {
            if (!intr.expired()) {
                if (-1 == proc(mname, intr.lock())) {
                    return -1;
                }
            }
        });

        return 0;
    }

 protected:
    IQuerier *querier_;
};
}  // namespace flowsql

#define BEGIN_MOD_REGIST(classname)                                                           \
    const char *classname::Name() const { return #classname; }                                \
    EXPORT_API const char *RegisterInterface(fast::IRegister *registry, const char *option) { \
        classname *_module = new classname;                                                   \
        const char *mname = #classname;                                                       \
        if (-1 == registry->Register(std::shared_ptr<fast::IModule>(_module))) {              \
            return nullptr;                                                                   \
        }

#define ____INTERFACE(iid, intername)                  \
    intername *i = dynamic_cast<intername *>(_module); \
    registry->Register(mname, iid, i);

#define END_MOD_REGIST()     \
    _module->Option(option); \
    return mname;            \
    }

/****************************************************
                      HOW TO USE

// sample.h
#include <base/modularity/mbasic.h>
class SampleModule : public fast::Mbasic, fast::ICapture
{
...
};

// sample.cpp
#include "sample.h"
BEGIN_MOD_REGIST(SampleModule)
____INTERFACE(IID_CAPTURE, fast::ICapture)
END_MOD_REGIST
****************************************************/

#endif  //_FAST_COMMON_MODULARITY_MBASIC_H_
