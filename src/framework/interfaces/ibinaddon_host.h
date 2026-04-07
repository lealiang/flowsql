/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IBINADDON_HOST_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IBINADDON_HOST_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <string>

namespace flowsql {

// {0x5a9d193e-59fd-4610-b8ca-5bf0fd15f2ba}
const Guid IID_BINADDON_HOST = {
    0x5a9d193e, 0x59fd, 0x4610, {0xb8, 0xca, 0x5b, 0xf0, 0xfd, 0x15, 0xf2, 0xba}};

/**
 * @brief BinAddon 宿主接口，供 CatalogPlugin 将 C++ 插件管理请求统一委派到宿主实现。
 */
interface IBinAddonHost {
    virtual ~IBinAddonHost() = default;

    /**
     * @brief 查询已安装 C++ 插件列表。
     * @param rsp 输出 JSON 响应字符串。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int ListCppPlugins(std::string& rsp) = 0;
    /**
     * @brief 上传 C++ 插件文件并写入插件仓库。
     * @param filename 上传时的原始文件名。
     * @param tmp_path 临时文件路径。
     * @param rsp 输出 JSON 响应字符串。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int UploadCppPlugin(const std::string& filename, const std::string& tmp_path, std::string& rsp) = 0;
    /**
     * @brief 激活指定插件，使其进入可用状态。
     * @param plugin_id 插件唯一标识。
     * @param rsp 输出 JSON 响应字符串。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int ActivateCppPlugin(const std::string& plugin_id, std::string& rsp) = 0;
    /**
     * @brief 停用指定插件，使其不再参与运行。
     * @param plugin_id 插件唯一标识。
     * @param rsp 输出 JSON 响应字符串。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int DeactivateCppPlugin(const std::string& plugin_id, std::string& rsp) = 0;
    /**
     * @brief 删除指定插件及其元数据。
     * @param plugin_id 插件唯一标识。
     * @param rsp 输出 JSON 响应字符串。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int DeleteCppPlugin(const std::string& plugin_id, std::string& rsp) = 0;
    /**
     * @brief 查询指定插件详情。
     * @param plugin_id 插件唯一标识。
     * @param rsp 输出 JSON 响应字符串。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int GetCppPluginDetail(const std::string& plugin_id, std::string& rsp) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBINADDON_HOST_H_
