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
#ifndef _FLOWSQL_COMMON_IPC_SERVER_H_
#define _FLOWSQL_COMMON_IPC_SERVER_H_

#include "common.h"

namespace flowsql {
namespace ipc {
typedef std::function<void(void)> RecvTimeoutNotifyCallback;

class Server : public Common {
 public:
    Server() : running_(false) {}

    ~Server() {}

    bool start(const char *addr, const char *&err_msg, RecvCallback req_cb, int recv_timeout_ms = 1000,
               RecvTimeoutNotifyCallback notify_cb = NULL) {
        zmq_ctx_ = zmq_ctx_new();
        socket_ = zmq_socket(zmq_ctx_, ZMQ_REP);
        int rc = zmq_bind(socket_, addr);
        if (0 != rc) {
            err_msg = strerror(errno);
            return false;
        }

        zmq_setsockopt(socket_, ZMQ_RCVTIMEO, &recv_timeout_ms, sizeof(recv_timeout_ms));

        running_ = true;
        while (running_) {
            responsed_ = false;
            bool recevied = recv(req_cb);
            if (recevied && !responsed_) {
                zmq_send(socket_, 0, 0, 0);
            } else if (!recevied && notify_cb) {
                notify_cb();
            }
        }

        zmq_close(socket_);
        zmq_ctx_destroy(zmq_ctx_);

        return true;
    }

    void stop() { running_ = false; }

    bool response(const uint8_t *data, int len, bool is_last) {
        responsed_ = true;
        bool ret = send(data, len, is_last);
        return ret;
    }

 private:
    bool running_;
    bool responsed_;
};

}  // namespace ipc
}  // namespace flowsql

#endif
