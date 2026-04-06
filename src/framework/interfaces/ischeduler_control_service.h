#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ISCHEDULER_CONTROL_SERVICE_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ISCHEDULER_CONTROL_SERVICE_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <string>

namespace flowsql {

// {0x3f8db2e1-0x93f0-0x4f4f-{0xa7,0xd2,0x1e,0x5b,0x6c,0x8d,0x9f,0x10}}
const Guid IID_SCHEDULER_CONTROL_SERVICE = {
    0x3f8db2e1, 0x93f0, 0x4f4f, {0xa7, 0xd2, 0x1e, 0x5b, 0x6c, 0x8d, 0x9f, 0x10}};

interface ISchedulerControlService {
    virtual ~ISchedulerControlService() = default;

    virtual int32_t ClassifySql(const std::string& req_json, std::string* rsp_json) = 0;
    virtual int32_t ExecuteBatch(const std::string& req_json, std::string* rsp_json) = 0;
    virtual int32_t ExecuteStream(const std::string& req_json, std::string* rsp_json) = 0;
    virtual int32_t StopStream(const std::string& req_json, std::string* rsp_json) = 0;
    virtual int32_t QueryStreamStatus(const std::string& req_json, std::string* rsp_json) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ISCHEDULER_CONTROL_SERVICE_H_
