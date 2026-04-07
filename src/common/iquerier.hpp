/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_COMMON_IQUERIER_HPP_
#define _FLOWSQL_COMMON_IQUERIER_HPP_
#include <common/guid.h>
#include <common/typedef.h>
#include <functional>

namespace flowsql {

/**
 * @brief 进程内接口查询器，用于在插件间发现与遍历接口实例。
 */
interface IQuerier {
    typedef std::function<int(void*)> fntraverse;
    /**
     * @brief 遍历指定接口类型的所有实例。
     * @param iid 目标接口 IID。
     * @param proc 遍历回调；参数为接口实例指针（void*），返回非 0 可中断遍历。
     * @return 0 表示遍历完成，非 0 表示回调或遍历异常。
     */
    virtual int Traverse(const Guid& iid, fntraverse proc) = 0;
    /**
     * @brief 返回指定接口类型的首个实例。
     * @param iid 目标接口 IID。
     * @return 找到时返回实例指针（非拥有语义），未找到返回 nullptr。
     */
    virtual void* First(const Guid& iid) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_COMMON_IQUERIER_HPP_
