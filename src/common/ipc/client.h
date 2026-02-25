/*
 * Copyright (C) 2020-06 - FATEAM
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 *
 * Author       : li_wangyang@163.com
 * Date         : 2020-07-07 06:56:56
 * LastEditors  : li_wangyang@163.com
 * LastEditTime : 2020-09-10 05:46:20
 */
#ifndef _BASE_IPC_CLIENT_H_
#define _BASE_IPC_CLIENT_H_

#include "common.h"

namespace flowsql {
namespace ipc {
class Client : public Common {
 public:
    Client() : last_recved_(true) {}

    ~Client() { disconnect(); }

    bool connect(const char *addr, const char *&err_msg) {
        disconnect();

        zmq_ctx_ = zmq_ctx_new();
        socket_ = zmq_socket(zmq_ctx_, ZMQ_REQ);
        int rc = zmq_connect(socket_, addr);
        if (0 != rc) {
            err_msg = strerror(errno);
            return false;
        }

        strcpy(addr_, addr);
        return true;
    }

    void disconnect() {
        if (socket_) {
            zmq_disconnect(socket_, addr_);
            zmq_close(socket_);
            socket_ = NULL;
        }

        if (zmq_ctx_) {
            zmq_ctx_shutdown(zmq_ctx_);
            zmq_ctx_ = NULL;
        }
    }

    bool request(const uint8_t *data, int len, bool is_last, int timeout_ms, RecvCallback rsp_cb) {
        if (!last_recved_) {
            zmq_setsockopt(socket_, ZMQ_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
            last_recved_ = recv(rsp_cb);
            if (!last_recved_) {
                return false;
            }
        }

        // 发送请求
        bool send_ret = send(data, len, is_last);
        if (!send_ret) {
            return false;
        }

        if (!is_last) {
            return true;
        }

        // 接收应答
        zmq_setsockopt(socket_, ZMQ_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
        last_recved_ = recv(rsp_cb);
        return last_recved_;
    }

 private:
    bool last_recved_;
    char addr_[24];
};
}  // namespace ipc
}  // namespace flowsql

#endif
