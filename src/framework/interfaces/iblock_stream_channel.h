/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_STREAM_CHANNEL_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_STREAM_CHANNEL_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <cstdint>
#include <memory>

#include "ichannel.h"

namespace arrow {
class RecordBatch;
}

namespace flowsql {

// {0x7b8c9d0e-1f23-4567-89ab-cdef01234567}
const Guid IID_BLOCK_STREAM_CHANNEL = {0x7b8c9d0e, 0x1f23, 0x4567,
                                       {0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67}};

struct BlockPollEvent {
    enum Kind {
        kData,
        kTimeout,
        kEof,
        kCancelled,
        kError,
    };
    Kind kind = kTimeout;
    std::shared_ptr<arrow::RecordBatch> batch;
    int err = 0;
};

/**
 * @brief 块式流通道接口。
 */
interface IBlockStreamChannel : public IChannel {
    virtual ~IBlockStreamChannel() = default;

    /**
     * @brief 轮询读取一个数据块。
     * @param timeout_ms 超时时间（毫秒），0 表示不等待。
     * @return 块轮询事件（数据/超时/EOF/取消/错误）。
     */
    virtual BlockPollEvent PollBlock(int timeout_ms = 100) = 0;
    /**
     * @brief 释放已消费的数据块。
     * @param block 待释放的数据块。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& block) = 0;
    /**
     * @brief 取消通道消费。
     */
    virtual void Cancel() = 0;
    /**
     * @brief 判断通道是否已结束。
     * @return true 表示结束，false 表示仍有数据或仍在生产。
     */
    virtual bool IsFinished() const = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_STREAM_CHANNEL_H_
