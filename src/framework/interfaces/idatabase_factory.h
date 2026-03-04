#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_FACTORY_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_FACTORY_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <functional>

#include "idatabase_channel.h"

namespace flowsql {

// {0xa9b8c7d6, 0xe5f4, 0x3210, {0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10}}
const Guid IID_DATABASE_FACTORY = {0xa9b8c7d6, 0xe5f4, 0x3210,
                                   {0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10}};

// IDatabaseFactory — 数据库通道工厂接口
// 管理数据库通道的创建、缓存和释放
interface IDatabaseFactory {
    virtual ~IDatabaseFactory() = default;

    // 获取或创建数据库通道实例（懒加载）
    // type: "sqlite", "mysql", "clickhouse"
    // name: 通道名称（如 "mydb"）
    // 返回: 通道指针（工厂持有所有权），失败返回 nullptr
    virtual IDatabaseChannel* Get(const char* type, const char* name) = 0;

    // [预留] 获取数据库通道（带用户上下文）
    virtual IDatabaseChannel* GetWithContext(const char* type, const char* name,
                                             const char* user_context) {
        return Get(type, name);
    }

    // 列出所有已配置的数据库连接
    virtual void List(std::function<void(const char* type, const char* name)> callback) = 0;

    // 释放指定通道（关闭连接，从池中移除）
    virtual int Release(const char* type, const char* name) = 0;

    // 获取最近一次操作的错误信息（线程安全：thread_local 存储）
    virtual const char* LastError() = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_FACTORY_H_
