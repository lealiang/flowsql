/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ITASK_STORE_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ITASK_STORE_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <cstdint>
#include <string>
#include <vector>

namespace flowsql {

// {3f5b7601-92a4-4c5d-8b9d-26f5bb6ebd31}
const Guid IID_TASK_STORE = {
    0x3f5b7601, 0x92a4, 0x4c5d, {0x8b, 0x9d, 0x26, 0xf5, 0xbb, 0x6e, 0xbd, 0x31}
};

enum class TaskStatus : int32_t {
    kPending = 0,
    kRunning = 1,
    kCompleted = 2,
    kFailed = 3,
    kCancelled = 4,
    kTimeout = 5,
    kStopped = 6,
};

struct TaskRecord {
    std::string task_id;
    std::string request_sql;
    std::string task_kind = "batch";
    std::string runtime_task_id;
    TaskStatus status = TaskStatus::kPending;
    int64_t result_row_count = 0;
    int64_t result_col_count = 0;
    std::string result_target;
    std::string error_code;
    std::string error_message;
    std::string error_stage;
    std::string created_at;
    std::string started_at;
    std::string updated_at;
    std::string finished_at;
};

/**
 * @brief 任务存储接口，封装任务元数据持久化与查询能力。
 */
interface ITaskStore {
    virtual ~ITaskStore() = default;

    /**
     * @brief 创建任务记录并生成任务 ID。
     * @param request_sql 原始 SQL 文本。
     * @param task_id 输出任务 ID。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int CreateTask(const std::string& request_sql, std::string* task_id) = 0;
    /**
     * @brief 更新任务状态与执行结果。
     * @param task_id 任务 ID。
     * @param new_status 目标状态。
     * @param error_code 错误码，成功时可为空。
     * @param error_message 错误详情，成功时可为空。
     * @param error_stage 出错阶段标识。
     * @param result_row_count 结果行数。
     * @param result_col_count 结果列数。
     * @param result_target 结果落地目标（例如 sink 通道）。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int UpdateStatus(const std::string& task_id,
                             TaskStatus new_status,
                             const std::string& error_code,
                             const std::string& error_message,
                             const std::string& error_stage,
                             int64_t result_row_count,
                             int64_t result_col_count,
                             const std::string& result_target) = 0;
    /**
     * @brief 查询单个任务记录。
     * @param task_id 任务 ID。
     * @param out 输出任务记录。
     * @return 0 表示成功，非 0 表示失败或未找到。
     */
    virtual int GetTask(const std::string& task_id, TaskRecord* out) = 0;
    /**
     * @brief 分页查询任务记录。
     * @param page 页码（从 1 开始）。
     * @param page_size 每页条数。
     * @param status_filter 状态过滤条件，空字符串表示不过滤。
     * @param items 输出任务列表。
     * @param total 输出总记录数。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int ListTasks(int page,
                          int page_size,
                          const std::string& status_filter,
                          std::vector<TaskRecord>* items,
                          int64_t* total) = 0;
    /**
     * @brief 删除指定任务记录。
     * @param task_id 任务 ID。
     * @return 0 表示成功，非 0 表示失败或未找到。
     */
    virtual int DeleteTask(const std::string& task_id) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ITASK_STORE_H_
