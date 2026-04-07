/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

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

enum class ProducerMode {
    SINGLE,
    MULTI,
};

enum class ConsumerMode {
    SINGLE,
    MULTI,
};

enum class OrderGuarantee {
    GLOBAL_FIFO,
    PER_PRODUCER_FIFO,
    PER_PARTITION_FIFO,
    NONE,
};

enum class BackpressurePolicy {
    DROP_ONLY,
    BLOCK_ONLY,
    DROP_OR_BLOCK,
};

struct StreamConcurrencyCaps {
    ProducerMode put_mode = ProducerMode::SINGLE;
    ConsumerMode poll_mode = ConsumerMode::SINGLE;
    uint32_t max_producers = 1;  // 0 means unbounded
    uint32_t max_consumers = 1;  // 0 means unbounded
    bool lock_free_put = false;
    bool lock_free_poll = false;
    bool cancel_wakeup_guaranteed = false;
};

struct StreamSemanticCaps {
    bool finite = false;
    bool supports_timeout_poll = true;
    bool supports_filter_pushdown = false;
    bool filter_requires_full_match = true;
    bool eof_reliable = true;
    OrderGuarantee ordering = OrderGuarantee::NONE;
    BackpressurePolicy backpressure = BackpressurePolicy::DROP_OR_BLOCK;
};

struct StreamPartitionCaps {
    bool has_partition_id = false;
    bool supports_route_by_partition_id = false;
    bool preserves_partition_order = false;
};

struct StreamChannelCapabilities {
    std::string channel_type = ChannelType::kStream;
    StreamConcurrencyCaps concurrency;
    StreamSemanticCaps semantics;
    StreamPartitionCaps partition;
};

/**
 * @brief 流式通道接口，定义流数据投递、轮询消费与并发能力声明。
 */
interface IStreamChannel : public IChannel {
    virtual ~IStreamChannel() = default;

    /**
     * @brief 投递一个输入批次到流通道。
     * @param batch 输入批次数据。
     * @param ts_ms 批次时间戳（毫秒）。
     * @return 0 成功，EAGAIN 队列满，ECANCELED 已取消，<0 其他错误。
     */
    virtual int Put(std::shared_ptr<arrow::RecordBatch> batch, int64_t ts_ms) = 0;

    /**
     * @brief 轮询读取下一批数据。
     * @param timeout_ms 超时毫秒；0 表示立即返回，>0 表示等待。
     * @return 轮询事件（数据/超时/EOF/错误等）。
     */
    virtual PollEvent PollNext(int timeout_ms = 100) = 0;

    /**
     * @brief 获取通道输出 schema。
     * @return 静态 schema 指针；返回 nullptr 表示动态 schema。
     */
    virtual std::shared_ptr<arrow::Schema> GetOutputSchema() = 0;

    /**
     * @brief 设置过滤条件下推。
     * @param condition_json 过滤条件 JSON。
     * @param unsupported_out 输出不可下推条件列表，可为 nullptr。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int SetFilter(const char* condition_json,
                          std::vector<std::string>* unsupported_out) = 0;

    /**
     * @brief 判断通道是否已满。
     * @return true 表示已满。
     */
    virtual bool IsFull() const = 0;
    /**
     * @brief 判断通道是否为空。
     * @return true 表示为空。
     */
    virtual bool IsEmpty() const = 0;
    /**
     * @brief 获取通道容量。
     * @return 容量（元素个数）。
     */
    virtual size_t Capacity() const = 0;
    /**
     * @brief 获取当前通道长度。
     * @return 当前元素个数。
     */
    virtual size_t Size() const = 0;

    /**
     * @brief 判断是否为有限流。
     * @return true 表示有限流（可结束），false 表示无限流。
     */
    virtual bool IsFinite() const = 0;

    /**
     * @brief 关闭流并发送 EOF（有限流场景）。
     */
    virtual void CloseStream() = 0;

    /**
     * @brief 取消流处理并触发优雅退出。
     */
    virtual void Cancel() = 0;

    /**
     * @brief 查询生产/转发侧是否结束。
     * @return true 表示已结束。
     */
    virtual bool IsFinished() const = 0;

    /**
     * @brief 声明通道并发与语义能力。
     * @return 能力描述结构体。
     */
    virtual StreamChannelCapabilities Capabilities() const {
        StreamChannelCapabilities caps;
        caps.channel_type = ChannelType::kStream;
        caps.concurrency.put_mode = ProducerMode::SINGLE;
        caps.concurrency.poll_mode = ConsumerMode::SINGLE;
        caps.concurrency.max_producers = 1;
        caps.concurrency.max_consumers = 1;
        caps.concurrency.lock_free_put = false;
        caps.concurrency.lock_free_poll = false;
        caps.concurrency.cancel_wakeup_guaranteed = false;
        caps.semantics.finite = false;
        caps.semantics.supports_timeout_poll = true;
        caps.semantics.supports_filter_pushdown = false;
        caps.semantics.filter_requires_full_match = true;
        caps.semantics.eof_reliable = true;
        caps.semantics.ordering = OrderGuarantee::NONE;
        caps.semantics.backpressure = BackpressurePolicy::DROP_OR_BLOCK;
        caps.partition.has_partition_id = false;
        caps.partition.supports_route_by_partition_id = false;
        caps.partition.preserves_partition_order = false;
        return caps;
    }

    /**
     * @brief 是否支持 Hub 扩展能力。
     * @return true 表示支持 Hub 扩展。
     */
    virtual bool IsHubChannel() const { return false; }
    /**
     * @brief 返回 Hub 模式提示。
     * @return 模式字符串（如 split/merge），不支持时返回空串。
     */
    virtual const char* HubModeHint() const { return ""; }  // split|merge
    /**
     * @brief 返回 Hub 分区数。
     * @return 分区数，不支持时返回 0。
     */
    virtual size_t HubPartitionCount() const { return 0; }
    /**
     * @brief 返回指定 Hub 分区通道。
     * @param idx 分区索引。
     * @return 分区通道智能指针，不支持或越界时返回 nullptr。
     */
    virtual std::shared_ptr<IStreamChannel> HubPartition(size_t idx) const {
        (void)idx;
        return nullptr;
    }
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ISTREAM_CHANNEL_H_
