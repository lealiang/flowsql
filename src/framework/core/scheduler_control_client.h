/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_CORE_SCHEDULER_CONTROL_CLIENT_H_
#define _FLOWSQL_FRAMEWORK_CORE_SCHEDULER_CONTROL_CLIENT_H_

#include <common/error_code.h>
#include <common/iquerier.hpp>
#include <framework/interfaces/irouter_handle.h>
#include <framework/interfaces/ischeduler_control_service.h>

#include <string>

namespace flowsql {

class SchedulerControlClient {
 public:
    SchedulerControlClient() = default;
    explicit SchedulerControlClient(IQuerier* querier) : querier_(querier) {}

    void ResetQuerier(IQuerier* querier) { querier_ = querier; }

    int32_t ClassifySql(const std::string& req_json, std::string* rsp_json) const;
    int32_t ExecuteBatch(const std::string& req_json, std::string* rsp_json) const;
    int32_t SubmitBatch(const std::string& req_json, std::string* rsp_json) const;
    int32_t QueryBatchStatus(const std::string& req_json, std::string* rsp_json) const;
    int32_t StopBatch(const std::string& req_json, std::string* rsp_json) const;
    int32_t ExecuteStream(const std::string& req_json, std::string* rsp_json) const;
    int32_t StopStream(const std::string& req_json, std::string* rsp_json) const;
    int32_t QueryStreamStatus(const std::string& req_json, std::string* rsp_json) const;

 private:
    ISchedulerControlService* Acquire(std::string* err_rsp) const;

    IQuerier* querier_ = nullptr;
};

// Router adapter for tests/in-process dispatch that only has route handlers.
class RouterBackedSchedulerControlService : public ISchedulerControlService {
 public:
    explicit RouterBackedSchedulerControlService(IRouterHandle* router) : router_(router) {}

    void ResetRouter(IRouterHandle* router) { router_ = router; }

    int32_t ClassifySql(const std::string& req_json, std::string* rsp_json) override;
    int32_t ExecuteBatch(const std::string& req_json, std::string* rsp_json) override;
    int32_t SubmitBatch(const std::string& req_json, std::string* rsp_json) override;
    int32_t QueryBatchStatus(const std::string& req_json, std::string* rsp_json) override;
    int32_t StopBatch(const std::string& req_json, std::string* rsp_json) override;
    int32_t ExecuteStream(const std::string& req_json, std::string* rsp_json) override;
    int32_t StopStream(const std::string& req_json, std::string* rsp_json) override;
    int32_t QueryStreamStatus(const std::string& req_json, std::string* rsp_json) override;

 private:
    int32_t Dispatch(const char* uri, const std::string& req_json, std::string* rsp_json) const;

    IRouterHandle* router_ = nullptr;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_SCHEDULER_CONTROL_CLIENT_H_
