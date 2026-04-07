/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IROUTER_HANDLE_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IROUTER_HANDLE_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <functional>
#include <string>

namespace flowsql {

/**
 * @brief 路由处理函数签名（纯业务逻辑，不感知 HTTP 协议细节）。
 * @param uri 完整路由路径。
 * @param req_json 请求 JSON 字符串。
 * @param rsp_json 输出 JSON 字符串。
 * @return 业务错误码（0 表示成功）。
 */
typedef std::function<int32_t(const std::string& uri,
                               const std::string& req_json,
                               std::string& rsp_json)>
    fnRouterHandler;

// 路由条目
struct RouteItem {
    std::string method;   // "GET" / "POST" / "PUT" / "DELETE"
    std::string uri;      // 完整路径，如 "/tasks/instant/execute"
    fnRouterHandler handler;
};

// {0xa1b2c3d4-e5f6-7890-abcd-ef1234567890}
const Guid IID_ROUTER_HANDLE = {
    0xa1b2c3d4, 0xe5f6, 0x7890, {0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90}};

/**
 * @brief 插件路由声明接口，RouterAgencyPlugin 在启动阶段统一收集路由表。
 */
interface IRouterHandle {
    virtual ~IRouterHandle() = default;
    /**
     * @brief 枚举插件暴露的所有路由。
     * @param callback 枚举回调，参数为单条路由信息。
     */
    virtual void EnumRoutes(std::function<void(const RouteItem&)> callback) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IROUTER_HANDLE_H_
