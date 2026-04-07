/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_SERVICES_DATABASE_CAPABILITY_INTERFACES_H_
#define _FLOWSQL_SERVICES_DATABASE_CAPABILITY_INTERFACES_H_

#include <framework/interfaces/idatabase_channel.h>

#include <memory>
#include <string>
#include <vector>

namespace arrow {
class RecordBatch;
}

namespace flowsql {
namespace database {

class IDbSession;

/**
 * @brief 行式批量读取能力接口。
 */
interface IBatchReadable {
    virtual ~IBatchReadable() = default;
    /**
     * @brief 创建批量读取器。
     * @param query 查询 SQL。
     * @param reader 输出读取器指针。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int CreateReader(const char* query, IBatchReader** reader) = 0;
    /**
     * @brief 获取最近一次错误信息。
     * @return 错误字符串指针。
     */
    virtual const char* GetLastError() = 0;
};

/**
 * @brief 行式批量写入能力接口。
 */
interface IBatchWritable {
    virtual ~IBatchWritable() = default;
    /**
     * @brief 创建批量写入器。
     * @param table 目标表名。
     * @param writer 输出写入器指针。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int CreateWriter(const char* table, IBatchWriter** writer) = 0;
    /**
     * @brief 获取最近一次错误信息。
     * @return 错误字符串指针。
     */
    virtual const char* GetLastError() = 0;
};

/**
 * @brief Arrow 原生读取能力接口。
 */
interface IArrowReadable {
    virtual ~IArrowReadable() = default;
    /**
     * @brief 执行查询并返回 Arrow 批次。
     * @param sql 查询 SQL。
     * @param batches 输出批次数组。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int ExecuteQueryArrow(const char* sql,
                                  std::vector<std::shared_ptr<arrow::RecordBatch>>* batches) = 0;
    /**
     * @brief 获取最近一次错误信息。
     * @return 错误字符串指针。
     */
    virtual const char* GetLastError() = 0;
};

/**
 * @brief Arrow 原生写入能力接口。
 */
interface IArrowWritable {
    virtual ~IArrowWritable() = default;
    /**
     * @brief 写入 Arrow 批次数组到目标表。
     * @param table 目标表名。
     * @param batches 输入批次数组。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int WriteArrowBatches(const char* table,
                                  const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) = 0;
    /**
     * @brief 获取最近一次错误信息。
     * @return 错误字符串指针。
     */
    virtual const char* GetLastError() = 0;
};

/**
 * @brief 事务能力接口。
 */
interface ITransactional {
    virtual ~ITransactional() = default;
    /**
     * @brief 开启事务。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int BeginTransaction() = 0;
    /**
     * @brief 提交事务。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int CommitTransaction() = 0;
    /**
     * @brief 回滚事务。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int RollbackTransaction() = 0;
    /**
     * @brief 获取最近一次错误信息。
     * @return 错误字符串指针。
     */
    virtual const char* GetLastError() = 0;
};

/**
 * @brief 会话工厂能力接口。
 */
interface IDbSessionFactoryProvider {
    virtual ~IDbSessionFactoryProvider() = default;
    /**
     * @brief 创建数据库会话实例。
     * @return 会话智能指针。
     */
    virtual std::shared_ptr<IDbSession> CreateSession() = 0;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_CAPABILITY_INTERFACES_H_
