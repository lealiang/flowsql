/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ISCHEDULER_CONTROL_SERVICE_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ISCHEDULER_CONTROL_SERVICE_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <string>

namespace flowsql {

// {0x3f8db2e1-0x93f0-0x4f4f-{0xa7,0xd2,0x1e,0x5b,0x6c,0x8d,0x9f,0x10}}
const Guid IID_SCHEDULER_CONTROL_SERVICE = {
    0x3f8db2e1, 0x93f0, 0x4f4f, {0xa7, 0xd2, 0x1e, 0x5b, 0x6c, 0x8d, 0x9f, 0x10}};

/**
 * @brief Scheduler 控制服务接口，供 TaskPlugin 进行 SQL 分类与任务调度调用。
 */
interface ISchedulerControlService {
    virtual ~ISchedulerControlService() = default;

    /**
     * @brief 对 SQL 请求进行分类，判断应走 batch 还是 stream 执行。
     * @param req_json 输入 JSON，请求体。
     * @param rsp_json 输出 JSON，分类结果或错误信息。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int32_t ClassifySql(const std::string& req_json, std::string* rsp_json) = 0;
    /**
     * @brief 执行 batch 任务。
     * @param req_json 输入 JSON，请求体。
     * @param rsp_json 输出 JSON，执行结果。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int32_t ExecuteBatch(const std::string& req_json, std::string* rsp_json) = 0;
    /**
     * @brief 执行 stream 任务。
     * @param req_json 输入 JSON，请求体。
     * @param rsp_json 输出 JSON，执行结果。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int32_t ExecuteStream(const std::string& req_json, std::string* rsp_json) = 0;
    /**
     * @brief 停止 stream 任务。
     * @param req_json 输入 JSON，请求体。
     * @param rsp_json 输出 JSON，停止结果。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int32_t StopStream(const std::string& req_json, std::string* rsp_json) = 0;
    /**
     * @brief 查询 stream 任务状态。
     * @param req_json 输入 JSON，请求体。
     * @param rsp_json 输出 JSON，状态结果。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int32_t QueryStreamStatus(const std::string& req_json, std::string* rsp_json) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ISCHEDULER_CONTROL_SERVICE_H_
