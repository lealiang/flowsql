/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_HUB_CHANNEL_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_HUB_CHANNEL_H_

#include <framework/interfaces/istream_channel.h>

#include <memory>

namespace flowsql {

/**
 * @brief Stream Hub 接口，支持 split/merge 等多分区流通道能力。
 */
interface __attribute__((visibility("default"))) IStreamHubChannel : public IStreamChannel {
    /**
     * @brief 查询 Hub 模式。
     * @return 模式字符串（例如 "split"、"merge"）。
     */
    virtual const char* HubMode() const = 0;  // split|merge
    /**
     * @brief 获取分区数量。
     * @return 分区数量。
     */
    virtual size_t PartitionCount() const = 0;
    /**
     * @brief 获取指定分区通道。
     * @param idx 分区索引（从 0 开始）。
     * @return 分区通道智能指针；越界时返回 nullptr。
     */
    virtual std::shared_ptr<IStreamChannel> GetPartition(size_t idx) const = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_HUB_CHANNEL_H_
