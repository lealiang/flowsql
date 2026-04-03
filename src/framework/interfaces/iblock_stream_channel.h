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

interface IBlockStreamChannel : public IChannel {
    virtual ~IBlockStreamChannel() = default;

    virtual BlockPollEvent PollBlock(int timeout_ms = 100) = 0;
    virtual int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& block) = 0;
    virtual void Cancel() = 0;
    virtual bool IsFinished() const = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_STREAM_CHANNEL_H_
