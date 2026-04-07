/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_COMMON_IPLUGIN_H_
#define _FLOWSQL_COMMON_IPLUGIN_H_

#include "guid.h"
#include "typedef.h"
#include "iquerier.hpp"

namespace flowsql {

/**
 * @brief 插件注册接口，由加载器在插件注册阶段调用。
 */
interface IRegister {
    /**
     * @brief 注册插件暴露的接口实例。
     * @param iid 接口唯一标识。
     * @param iface 接口实例指针（非拥有语义）。
     */
    virtual void Regist(const Guid& iid, void* iface) = 0;
};

// {86dc3d8-e65f-9a83-1a39-66d26e95a9ca}
const Guid IID_PLUGIN = {0x86dc3d8, 0xe65f, 0x9a83, {0x1a, 0x39, 0x66, 0xd2, 0x6e, 0x95, 0xa9, 0xca}};

/**
 * @brief 插件生命周期接口，定义插件装载与运行阶段回调。
 */
interface IPlugin {
    virtual ~IPlugin(){};

    /**
     * @brief 解析插件配置参数。
     * @param arg 插件启动参数字符串，可为空。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Option(const char* /* arg */) { return 0; }
    /**
     * @brief 执行插件加载逻辑并绑定依赖接口。
     * @param querier 插件查询器，用于获取其他插件暴露的接口。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Load(IQuerier* querier) = 0;
    /**
     * @brief 执行插件卸载逻辑并释放资源。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Unload() = 0;

    /**
     * @brief 启动插件运行逻辑。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Start() { return 0; }
    /**
     * @brief 停止插件运行逻辑。
     * @return 0 表示成功，非 0 表示失败。
     */
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
