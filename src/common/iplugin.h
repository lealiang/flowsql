/*
 * Copyright (C) 2020-06 - flowSQL
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 * 插件接口定义 — 所有插件只需 include 此文件
 * IPlugin: 插件生命周期接口
 * IRegister: 插件注册接口（由 PluginLoader 实现）
 * BEGIN_PLUGIN_REGIST 宏: 插件注册入口
 */
#ifndef _FLOWSQL_COMMON_IPLUGIN_H_
#define _FLOWSQL_COMMON_IPLUGIN_H_

#include "guid.h"
#include "typedef.h"
#include "iquerier.hpp"

namespace flowsql {

interface IRegister {
    virtual void Regist(const Guid& iid, void* iface) = 0;
};

// {86dc3d8-e65f-9a83-1a39-66d26e95a9ca}
const Guid IID_PLUGIN = {0x86dc3d8, 0xe65f, 0x9a83, {0x1a, 0x39, 0x66, 0xd2, 0x6e, 0x95, 0xa9, 0xca}};

interface IPlugin {
    virtual ~IPlugin(){};

    virtual int Option(const char* /* arg */) { return 0; }
    virtual int Load(IQuerier* querier) = 0;  // 插件初始化，通过 querier 查询其他插件接口
    virtual int Unload() = 0;

    // 模块启停（默认空实现，轻量插件无需覆写）
    virtual int Start() { return 0; }
    virtual int Stop() { return 0; }
};

}  // namespace flowsql

// --- 插件注册宏 ---

#define BEGIN_PLUGIN_REGIST(classname)                                                          \
    EXPORT_API void pluginunregist() {}                                                         \
                                                                                                \
    EXPORT_API flowsql::IPlugin* pluginregist(flowsql::IRegister* registry, const char* opt) {  \
        static classname _plugin;

#define ____INTERFACE(iid, intername)                           \
    {                                                           \
        intername* iface = dynamic_cast<intername*>(&_plugin);  \
        registry->Regist(iid, iface);                           \
    }

#define END_PLUGIN_REGIST() \
    _plugin.Option(opt);    \
    return &_plugin;        \
    }

#endif  // _FLOWSQL_COMMON_IPLUGIN_H_
