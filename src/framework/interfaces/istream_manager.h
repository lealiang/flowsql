/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_MANAGER_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_MANAGER_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <functional>
#include <string>

namespace flowsql {

// {0xf1e2d3c4-b5a6-4789-8abc-def012345678}
const Guid IID_STREAM_MANAGER = {0xf1e2d3c4, 0xb5a6, 0x4789,
                                 {0x8a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78}};

/**
 * @brief 流通道管理接口，负责 stream 通道的增删改查。
 */
interface IStreamManager {
    virtual ~IStreamManager() = default;

    /**
     * @brief 新增流通道配置并注册通道。
     * @param type 通道类型。
     * @param name 通道名称。
     * @param option 通道配置 JSON 字符串。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int AddChannel(const std::string& type,
                           const std::string& name,
                           const std::string& option) = 0;
    /**
     * @brief 修改已有流通道配置。
     * @param type 通道类型。
     * @param name 通道名称。
     * @param option 通道配置 JSON 字符串。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int ModifyChannel(const std::string& type,
                              const std::string& name,
                              const std::string& option) = 0;
    /**
     * @brief 删除流通道配置并反注册通道。
     * @param type 通道类型。
     * @param name 通道名称。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int RemoveChannel(const std::string& type,
                              const std::string& name) = 0;

    /**
     * @brief 查询流通道列表。
     * @param callback 枚举回调，参数依次为 type、name、option、status。
     */
    virtual void QueryChannels(std::function<void(const std::string& type,
                                                  const std::string& name,
                                                  const std::string& option,
                                                  const std::string& status)> callback) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_MANAGER_H_
