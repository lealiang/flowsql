/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2020-07-07 06:56:56
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
#ifndef _FLOWSQL_COMMON_IPC_COMMON_H_
#define _FLOWSQL_COMMON_IPC_COMMON_H_

#include <string.h>
#include <unistd.h>
#include <zmq.h>
#include <functional>
#include <iostream>

namespace flowsql {
namespace ipc {
#define ZMQ_BUFFER_SIZE 65536

// 接收数据回调函数
typedef std::function<void(uint8_t *data, int len, bool is_last)> RecvCallback;

class Common {
 protected:
    Common() : zmq_ctx_(NULL), socket_(NULL), block_buffer_(NULL) { block_buffer_ = new uint8_t[ZMQ_BUFFER_SIZE]; }

    ~Common() {
        if (block_buffer_) {
            delete[] block_buffer_;
            block_buffer_ = NULL;
        }
    }

    bool send(const uint8_t *data, int len, bool is_last) {
        // 先按块发送
        int sent_bytes = 0;
        while (sent_bytes + ZMQ_BUFFER_SIZE <= len) {
            if (-1 != zmq_send(socket_, data + sent_bytes, ZMQ_BUFFER_SIZE, ZMQ_SNDMORE)) {
                sent_bytes += ZMQ_BUFFER_SIZE;
                continue;
            }
            return false;
        }

        // 发送最后剩余部分
        if (sent_bytes < len) {
            if (-1 == zmq_send(socket_, data + sent_bytes, len - sent_bytes, ZMQ_SNDMORE)) {
                return false;
            }
        }

        // 发送结束通知
        if (is_last && -1 == zmq_send(socket_, 0, 0, ZMQ_DONTWAIT)) {
            return false;
        }

        return true;
    }

    bool recv(RecvCallback &recv_cb) {
        while (1) {
            // memset(block_buffer_, 0, ZMQ_BUFFER_SIZE);
            int len = zmq_recv(socket_, block_buffer_, ZMQ_BUFFER_SIZE, 0);
            if (-1 == len) {
                return false;
            }

            if (recv_cb) {
                recv_cb(block_buffer_, len, 0 == len);
            }

            if (0 == len) {
                return true;
            }
        }
    }

 protected:
    void *zmq_ctx_;
    void *socket_;
    uint8_t *block_buffer_;
};
}  // namespace ipc
}  // namespace flowsql

#endif
