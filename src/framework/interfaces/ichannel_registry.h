/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ICHANNEL_REGISTRY_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ICHANNEL_REGISTRY_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <functional>
#include <memory>

#include "ichannel.h"

namespace flowsql {

// {0xb6f1c3d2-8a31-4e63-bb65-11a17f0d9e41}
const Guid IID_CHANNEL_REGISTRY = {
    0xb6f1c3d2, 0x8a31, 0x4e63, {0xbb, 0x65, 0x11, 0xa1, 0x7f, 0x0d, 0x9e, 0x41}};

/**
 * @brief 具名通道注册中心接口。
 *
 * name 不含 catalog 前缀（例如 "result"，而非 "dataframe.result"）。
 */
interface IChannelRegistry {
    virtual ~IChannelRegistry() = default;

    /**
     * @brief 注册具名通道。
     * @param name 通道名称（不含 catalog 前缀）。
     * @param channel 通道实例智能指针。
     * @return 0 表示成功，非 0 表示失败（如同名已存在）。
     */
    virtual int Register(const char* name, std::shared_ptr<IChannel> channel) = 0;

    /**
     * @brief 按名称查找通道。
     * @param name 通道名称。
     * @return 通道实例智能指针；未找到返回 nullptr。
     */
    virtual std::shared_ptr<IChannel> Get(const char* name) = 0;

    /**
     * @brief 注销通道。
     * @param name 通道名称。
     * @return 0 表示成功，非 0 表示失败（如未注册）。
     */
    virtual int Unregister(const char* name) = 0;

    /**
     * @brief 原子重命名通道。
     * @param old_name 旧名称。
     * @param new_name 新名称。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Rename(const char* old_name, const char* new_name) = 0;

    /**
     * @brief 枚举所有已注册通道。
     * @param callback 枚举回调，参数为通道名称和通道实例。
     */
    virtual void List(std::function<void(const char* name, std::shared_ptr<IChannel>)> callback) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ICHANNEL_REGISTRY_H_
