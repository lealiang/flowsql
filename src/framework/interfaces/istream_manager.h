#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_MANAGER_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_MANAGER_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <functional>
#include <string>

namespace flowsql {

// {0xf1e2d3c4-b5a6-4789-8abc-def012345678}
const Guid IID_STREAM_MANAGER = {0xf1e2d3c4, 0xb5a6, 0x4789,
                                 {0x8a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78}};

interface IStreamManager {
    virtual ~IStreamManager() = default;

    virtual int AddChannel(const std::string& type,
                           const std::string& name,
                           const std::string& option) = 0;
    virtual int ModifyChannel(const std::string& type,
                              const std::string& name,
                              const std::string& option) = 0;
    virtual int RemoveChannel(const std::string& type,
                              const std::string& name) = 0;

    virtual void QueryChannels(std::function<void(const std::string& type,
                                                  const std::string& name,
                                                  const std::string& option,
                                                  const std::string& status)> callback) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_MANAGER_H_
