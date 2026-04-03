#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_CHANNEL_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_CHANNEL_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ichannel.h"

namespace arrow {
class RecordBatch;
class Schema;
}

namespace flowsql {

// {0xb4c5d6e7-f809-1a2b-3c4d-5e6f708192a3}
const Guid IID_STREAM_CHANNEL = {0xb4c5d6e7, 0xf809, 0x1a2b,
                                 {0x3c, 0x4d, 0x5e, 0x6f, 0x70, 0x81, 0x92, 0xa3}};

// 路径 A 的核心传输单元（结构化列式流）
struct StreamBatch {
    std::shared_ptr<arrow::RecordBatch> data;
    int64_t ts_ms = 0;       // 采集时间戳（epoch ms）
    int32_t partition_id = -1;  // 预分区路由标识（-1 表示未设置）
    bool is_eof = false;     // 流结束控制帧
};

enum class PollEventKind {
    kData,
    kTimeout,
    kEof,
    kDrainedAfterCancel,
    kError,
};

struct PollEvent {
    PollEventKind kind;
    StreamBatch batch{};      // kind == kData 时有效
    int err = 0;              // kind == kError 时有效
    std::string err_msg;      // kind == kError 时有效

    static PollEvent Data(StreamBatch b) {
        PollEvent ev{PollEventKind::kData};
        ev.batch = std::move(b);
        return ev;
    }
    static PollEvent Timeout() { return PollEvent{PollEventKind::kTimeout}; }
    static PollEvent Eof() { return PollEvent{PollEventKind::kEof}; }
    static PollEvent DrainedAfterCancel() {
        return PollEvent{PollEventKind::kDrainedAfterCancel};
    }
    static PollEvent Error(int code, std::string msg) {
        PollEvent ev{PollEventKind::kError};
        ev.err = code;
        ev.err_msg = std::move(msg);
        return ev;
    }
};

// IStreamChannel — 流式通道接口（路径 A）
interface IStreamChannel : public IChannel {
    virtual ~IStreamChannel() = default;

    // 生产者（通道内部采集线程调用）
    // 返回 0=成功，EAGAIN=队列满，ECANCELED=通道已取消，<0=其他错误
    virtual int Put(std::shared_ptr<arrow::RecordBatch> batch, int64_t ts_ms) = 0;

    // 消费者（worker 调用）
    // timeout_ms: 0=立即返回，>0=等待指定毫秒
    virtual PollEvent PollNext(int timeout_ms = 100) = 0;

    // 返回静态输出 schema；返回 null 表示动态 schema
    virtual std::shared_ptr<arrow::Schema> GetOutputSchema() = 0;

    // WHERE 下推，unsupported_out 非空时表示存在不可下推条件
    virtual int SetFilter(const char* condition_json,
                          std::vector<std::string>* unsupported_out) = 0;

    // 队列状态
    virtual bool IsFull() const = 0;
    virtual bool IsEmpty() const = 0;
    virtual size_t Capacity() const = 0;
    virtual size_t Size() const = 0;

    // 是否有限流（例如 pcap 文件）
    virtual bool IsFinite() const = 0;

    // 有限流内部使用：读完后推 EOF
    virtual void CloseStream() = 0;

    // 外部取消：触发优雅收敛退出
    virtual void Cancel() = 0;

    // true 表示生产/转发侧已结束
    virtual bool IsFinished() const = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_CHANNEL_H_
