/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_SERVICES_DATABASE_IDB_DRIVER_H_
#define _FLOWSQL_SERVICES_DATABASE_IDB_DRIVER_H_

#include <common/typedef.h>

#include <string>
#include <unordered_map>

namespace flowsql {
namespace database {

/**
 * @brief 数据库驱动基础接口。
 *
 * 只定义连接管理与元数据查询能力；数据读写由能力接口按需组合。
 */
interface IDbDriver {
    virtual ~IDbDriver() = default;

    /**
     * @brief 建立数据库连接。
     * @param params 连接参数映射（host/user/password/db 等）。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Connect(const std::unordered_map<std::string, std::string>& params) = 0;
    /**
     * @brief 断开数据库连接。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Disconnect() = 0;
    /**
     * @brief 查询当前连接状态。
     * @return true 表示已连接，false 表示未连接。
     */
    virtual bool IsConnected() = 0;

    /**
     * @brief 获取驱动名称。
     * @return 驱动名称字符串。
     */
    virtual const char* DriverName() = 0;
    /**
     * @brief 获取最近一次错误信息。
     * @return 错误字符串指针。
     */
    virtual const char* LastError() = 0;

    /**
     * @brief 执行健康检查。
     * @return true 表示可用，false 表示不可用。
     */
    virtual bool Ping() = 0;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_IDB_DRIVER_H_
