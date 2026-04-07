/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IOPERATOR_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IOPERATOR_H_

#include <common/guid.h>
#include <common/span.h>
#include <common/typedef.h>

#include <cstdint>
#include <string>

namespace flowsql {

// 前向声明
interface IChannel;

// {0xd4e5f6a7-bcde-f012-3456-789abcdef012}
const Guid IID_OPERATOR = {0xd4e5f6a7, 0xbcde, 0xf012, {0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x12}};

enum class OperatorPosition : int32_t {
    STORAGE = 0,
    DATA = 1
};

/**
 * @brief 通用算子接口，定义元数据、配置与执行入口。
 */
interface IOperator {
    virtual ~IOperator() = default;

    /**
     * @brief 获取算子分类。
     * @return 分类字符串。
     */
    virtual std::string Category() = 0;
    /**
     * @brief 获取算子名称。
     * @return 名称字符串。
     */
    virtual std::string Name() = 0;
    /**
     * @brief 获取算子描述信息。
     * @return 描述字符串。
     */
    virtual std::string Description() = 0;
    /**
     * @brief 获取算子所在位置（数据面或存储面）。
     * @return 算子位置枚举值。
     */
    virtual OperatorPosition Position() = 0;

    /**
     * @brief 执行单输入算子逻辑。
     * @param in 输入通道指针（非拥有语义）。
     * @param out 输出通道指针（非拥有语义）。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Work(IChannel* in, IChannel* out) = 0;

    /**
     * @brief 执行多输入算子逻辑。
     * @param inputs 输入通道数组视图。
     * @param out 输出通道指针（非拥有语义）。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Work(Span<IChannel*> inputs, IChannel* out) {
        if (inputs.empty()) return -1;
        return Work(inputs[0], out);
    }

    /**
     * @brief 设置算子配置项。
     * @param key 配置键。
     * @param value 配置值。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Configure(const char* key, const char* value) = 0;

    /**
     * @brief 获取最近一次执行错误信息。
     * @return 错误字符串；无错误时可返回空串。
     */
    virtual std::string LastError() { return ""; }
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IOPERATOR_H_
