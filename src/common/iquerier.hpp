/*
 * Copyright (C) 2020-06 - flowSQL
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 * IQuerier — 插件查询接口
 * 插件通过 IQuerier 遍历或获取其他插件提供的接口
 */
#ifndef _FLOWSQL_COMMON_IQUERIER_HPP_
#define _FLOWSQL_COMMON_IQUERIER_HPP_
#include <common/guid.h>
#include <common/typedef.h>
#include <functional>

namespace flowsql {

interface IQuerier {
    typedef std::function<int(void*)> fntraverse;
    virtual int Traverse(const Guid& iid, fntraverse proc) = 0;
    virtual void* First(const Guid& iid) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_COMMON_IQUERIER_HPP_
