/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IBRIDGE_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IBRIDGE_H_

#include <common/iplugin.h>

#include <functional>
#include <memory>
#include <string>

namespace flowsql {

// {0xa1b2c3d4, 0xe5f6, 0x7890, {0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89}}
const Guid IID_BRIDGE = {0xa1b2c3d4, 0xe5f6, 0x7890, {0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89}};

interface IOperator;  // 前向声明

/**
 * @brief Python 算子桥接接口，提供算子发现与刷新能力。
 */
interface IBridge {
    virtual ~IBridge() {}
    /**
     * @brief 按分类和名称查找算子。
     * @param category 算子分类。
     * @param name 算子名称。
     * @return 算子智能指针；未找到返回空指针。
     */
    virtual std::shared_ptr<IOperator> FindOperator(const std::string& category, const std::string& name) = 0;
    /**
     * @brief 遍历所有已发现算子。
     * @param fn 遍历回调，参数为算子指针，返回 -1 可提前中断遍历。
     */
    virtual void TraverseOperators(std::function<int(IOperator*)> fn) = 0;
    /**
     * @brief 从 Python Worker 重新发现算子并刷新缓存。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Refresh() = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBRIDGE_H_
