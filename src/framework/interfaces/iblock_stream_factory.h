// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_STREAM_FACTORY_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_STREAM_FACTORY_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <functional>

#include "iblock_stream_channel.h"

namespace flowsql {

const Guid IID_BLOCK_STREAM_FACTORY = {
    0x2e8f1a4c, 0x6d71, 0x4f92, {0x91, 0x3a, 0xc7, 0x5d, 0x20, 0x8b, 0x64, 0xe1}};

/** Public discovery contract for block-stream channel providers. */
interface IBlockStreamFactory {
    virtual ~IBlockStreamFactory() = default;

    // Returned channels are provider-owned and remain valid while the provider is loaded.
    virtual IBlockStreamChannel* Get(const char* type, const char* name) = 0;
    virtual void List(std::function<void(const char* type,
                                         const char* name,
                                         IBlockStreamChannel*)> callback) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_STREAM_FACTORY_H_
