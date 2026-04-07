/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_FACTORY_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_FACTORY_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <functional>

namespace flowsql {

interface IStreamChannel;

// {0xe5f6a7b8-cdef-0123-4567-89abcdef0123}
const Guid IID_STREAM_FACTORY = {0xe5f6a7b8, 0xcdef, 0x0123,
                                 {0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23}};

/**
 * @brief 流式通道工厂接口，提供流通道查找与枚举能力。
 */
interface IStreamFactory {
    virtual ~IStreamFactory() = default;

    /**
     * @brief 按类型和名称查找流通道。
     * @param type 通道类型。
     * @param name 通道名称。
     * @return 通道指针（工厂持有所有权），失败返回 nullptr。
     */
    virtual IStreamChannel* Get(const char* type, const char* name) = 0;

    /**
     * @brief 枚举所有已注册流通道。
     * @param callback 枚举回调，参数依次为 type、name、channel。
     */
    virtual void List(std::function<void(const char* type, const char* name,
                                         IStreamChannel*)> callback) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_FACTORY_H_
