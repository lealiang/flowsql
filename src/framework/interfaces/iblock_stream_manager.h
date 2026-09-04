// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_STREAM_MANAGER_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_STREAM_MANAGER_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <functional>
#include <string>

namespace flowsql {

const Guid IID_BLOCK_STREAM_MANAGER = {
    0x7b3d5e90, 0x1a42, 0x4c86, {0xb4, 0x0f, 0x72, 0xd9, 0x5e, 0x31, 0xa8, 0x6c}};

/** Public runtime configuration contract for block-stream channel providers. */
interface IBlockStreamManager {
    virtual ~IBlockStreamManager() = default;

    virtual int AddChannel(const std::string& type,
                           const std::string& name,
                           const std::string& option) = 0;
    virtual int ModifyChannel(const std::string& type,
                              const std::string& name,
                              const std::string& option) = 0;
    virtual int RemoveChannel(const std::string& type,
                              const std::string& name) = 0;
    virtual void QueryChannels(
        std::function<void(const std::string& type,
                           const std::string& name,
                           const std::string& option,
                           const std::string& status)> callback) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_STREAM_MANAGER_H_
