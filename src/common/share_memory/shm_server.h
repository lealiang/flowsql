/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2020-09-10 02:11:37
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */

#ifndef _BASE_SHM_SERVER_H_
#define _BASE_SHM_SERVER_H_

#include <sys/shm.h>
#include <map>
#include <set>
#include "concurrentqueue/concurrentqueue.h"
#include "ipc/server.h"
#include "shm_typedef.h"
#include "threadsafe/lock.h"
#include "threadsafe/thread.h"

namespace flowsql {
namespace shm {

class Server {
    typedef std::function<void(uint8_t *data, int len)> ProcessCallback;

 public:
    Server() : last_err_msg_(NULL), running_(true) {}
    ~Server() { Stop(); }

    bool Start(const char *ipc_addr, int32_t process_thread_cnt, ProcessCallback process_cb) {
        process_cb_ = process_cb;
        for (int32_t i = 0; i < process_thread_cnt; ++i) {
            process_threads_.push_back(new thread(Server::ProcessThreadProc, this));
        }

        InitShmRspInfo init_info(-1, ERR_EXCEPTION);
        SubmitBlockRspInfo submit_info(-1, ERR_EXCEPTION);
        RecycleBlockRspInfo recycle_info(-1, ERR_EXCEPTION);
        DestoryShmRspInfo destory_info(-1, ERR_EXCEPTION);

        BaseRspInfo *rsp_infos[CMD_MAX];
        rsp_infos[CMD_INIT] = &init_info;
        rsp_infos[CMD_SUBMIT] = &submit_info;
        rsp_infos[CMD_RECYCLE] = &recycle_info;
        rsp_infos[CMD_DESTORY] = &destory_info;

        int32_t rsp_infos_len[CMD_MAX];
        rsp_infos_len[CMD_INIT] = sizeof(init_info);
        rsp_infos_len[CMD_SUBMIT] = sizeof(submit_info);
        rsp_infos_len[CMD_RECYCLE] = sizeof(recycle_info);
        rsp_infos_len[CMD_DESTORY] = sizeof(destory_info);

        const uint8_t *rsp_data = NULL;
        int rsp_len = 0;

        auto RequestCb = [&](uint8_t *data, int len, bool is_last) {
            if (len) {
                auto req_info = (BaseReqInfo *)data;
                int shm_id = req_info->shm_id;
                uint32_t cmd = req_info->cmd;

                if (CMD_MAX > cmd) {
                    BaseRspInfo *rsp_info = rsp_infos[cmd];
                    rsp_info->shm_id = shm_id;

                    rsp_data = (const uint8_t *)rsp_info;
                    rsp_len = rsp_infos_len[cmd];
                }

                switch (cmd) {
                    case CMD_INIT:
                        OnInitShmReq((InitShmReqInfo *)data, &init_info);
                        break;
                    case CMD_SUBMIT:
                        OnSubmitBlockReq((SubmitBlockReqInfo *)data, &submit_info);
                        break;
                    case CMD_RECYCLE:
                        OnRecycleBlockReq((RecycleBlockReqInfo *)data, &recycle_info);
                        break;
                    case CMD_DESTORY:
                        OnDestoryShmReq((DestoryShmReqInfo *)data, &destory_info);
                        break;
                    default:
                        rsp_data = NULL;
                        rsp_len = 0;
                        break;
                }
            } else {
                ipc_server_.response(rsp_data, rsp_len, true);
                rsp_data = NULL;
                rsp_len = 0;
            }
        };

        return ipc_server_.start(ipc_addr, last_err_msg_, RequestCb);
    }

    void Stop() {
        if (running_) {
            running_ = false;

            ipc_server_.stop();

            for (auto thd : process_threads_) {
                thd->join();
                delete thd;
            }

            for (auto queue : recv_block_) {
                delete queue.second;
            }

            for (auto queue : done_block_) {
                delete queue.second;
            }

            process_threads_.clear();
            recv_block_.clear();
            done_block_.clear();
        }
    }

    const char *last_err() const { return last_err_msg_; }

 private:
    static void ProcessThreadProc(Server *server) { server->OnProcessThread(); }

    void OnProcessThread() {
        bool has_data = true;
        while (running_) {
            if (!has_data) {
                usleep(1);
            }
            has_data = false;

            fast::rlocker rlock(shm_rwlocker_);
            for (auto it = recv_block_.begin(); it != recv_block_.end(); ++it) {
                int32_t shm_id = it->first;
                BlockQueue *queue = it->second;

                BlockInfo *info = NULL;
                if (queue->try_dequeue(info)) {
                    has_data = true;
                    process_cb_(info->GetBlock(), info->data_len);
                    done_block_[shm_id]->enqueue(info);
                }
            }
        }
    }

    void OnInitShmReq(InitShmReqInfo *req_info, InitShmRspInfo *rsp_info) {
        int32_t shm_id = req_info->shm_id;
        void *shm_addr = shmat(shm_id, 0, 0);
        if ((uint8_t *)-1 != shm_addr) {
            rsp_info->err = ERR_OK;

            fast::wlocker wlock(shm_rwlocker_);
            shm_addrs_[shm_id] = (uint8_t *)shm_addr;
            recv_block_[shm_id] = new BlockQueue(req_info->block_cnt);
            done_block_[shm_id] = new BlockQueue(req_info->block_cnt);
        } else {
            rsp_info->err = ERR_FAIL;
        }
    }

    void OnSubmitBlockReq(SubmitBlockReqInfo *req_info, SubmitBlockRspInfo *rsp_info) {
        int32_t shm_id = req_info->shm_id;
        rsp_info->offset = req_info->offset;

        fast::rlocker rlock(shm_rwlocker_);
        auto it = shm_addrs_.find(shm_id);
        if (it != shm_addrs_.end()) {
            rsp_info->err = ERR_OK;
            BlockInfo *info = (BlockInfo *)(it->second + req_info->offset);

            recv_block_[shm_id]->enqueue(info);

            // 处理不过来时
            // rsp_info.err = ERR_FAIL;
        } else {
            rsp_info->err = ERR_EXCEPTION;
        }
    }

    void OnRecycleBlockReq(RecycleBlockReqInfo *req_info, RecycleBlockRspInfo *rsp_info) {
        int32_t shm_id = req_info->shm_id;

        fast::rlocker rlock(shm_rwlocker_);
        auto it = shm_addrs_.find(shm_id);
        if (it != shm_addrs_.end()) {
            BlockInfo *info = NULL;
            if (done_block_[shm_id]->try_dequeue(info)) {
                rsp_info->err = ERR_OK;
                rsp_info->offset = (uint8_t *)info - it->second;
            } else {
                rsp_info->err = ERR_FAIL;
            }
        } else {
            rsp_info->err = ERR_EXCEPTION;
        }
    }

    void OnDestoryShmReq(DestoryShmReqInfo *req_info, DestoryShmRspInfo *rsp_info) {
        int32_t shm_id = req_info->shm_id;
        rsp_info->err = ERR_OK;

        fast::wlocker wlock(shm_rwlocker_);
        {
            auto it = recv_block_.find(shm_id);
            if (it != recv_block_.end()) {
                delete it->second;
                recv_block_.erase(it);
            }
        }
        {
            auto it = done_block_.find(shm_id);
            if (it != done_block_.end()) {
                delete it->second;
                done_block_.erase(it);
            }
        }
    }

 private:
    const char *last_err_msg_;
    bool running_;
    ipc::Server ipc_server_;

    std::vector<thread *> process_threads_;
    ProcessCallback process_cb_;

    rwlock shm_rwlocker_;
    std::map<int, uint8_t *> shm_addrs_;
    std::map<int, BlockQueue *> recv_block_;
    std::map<int, BlockQueue *> done_block_;
};

}  // namespace shm
}  // namespace flowsql

#endif
