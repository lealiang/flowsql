/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2020-08-17 03:30:32
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */

#ifndef _FLOWSQL_COMMON_SHARE_MEMORY_SHM_CLIENT_H_
#define _FLOWSQL_COMMON_SHARE_MEMORY_SHM_CLIENT_H_

#include <sys/shm.h>
#include <list>
#include <set>
#include "ipc/client.h"
#include "shm_typedef.h"
#include "threadsafe/lock.h"
#include "threadsafe/thread.h"

namespace flowsql {
namespace shm {

class Client {
 public:
    Client()
        : last_err_msg_(0),
          shm_addr_(0),
          shm_id_(-1),
          running_(true),
          thread_(NULL),
          free_queue_(NULL),
          to_send_queue_(NULL) {}

    ~Client() { Stop(); }

    bool Init(const char *ipc_addr, const char *shm_name, int64_t shm_bytes, int32_t block_bytes, int32_t timeoutsec) {
        last_err_msg_ = NULL;

        if (!ipc_client_.connect(ipc_addr, last_err_msg_)) {
            return false;
        }

        // 考虑到再次启动时，共享内存大小可能会变化，所以先通知remove old
        if (!RemoveOldShm(shm_name, timeoutsec)) {
            return false;
        }

        if (!InitShareMemory(shm_name, shm_bytes, block_bytes)) {
            return false;
        }

        thread_ = new thread(Client::SendThreadProc, this);

        return true;
    }

    // 从共享内存中分配一个块
    uint8_t *AllocBlock() {
        BlockInfo *info = NULL;
        return free_queue_ && free_queue_->try_dequeue(info) ? info->GetBlock() : NULL;
    }

    // 将块归还给共享内存，不发送
    void FreeBlock(uint8_t *block) { free_queue_->enqueue(GetBlockInfo(block)); }

    // 将块发送到另外一个进程
    void SendBlock(uint8_t *block, int32_t data_len) {
        BlockInfo *info = GetBlockInfo(block);
        info->data_len = data_len;

        to_send_queue_->enqueue(info);
    }

    void Stop() {
        if (running_) {
            running_ = false;
            if (thread_) {
                thread_->join();
                delete thread_;
                thread_ = NULL;
            }

            if (free_queue_) {
                delete free_queue_;
                free_queue_ = NULL;
            }

            if (to_send_queue_) {
                delete to_send_queue_;
                to_send_queue_ = NULL;
            }
        }
    }

    const char *LastErrMsg() const { return last_err_msg_; }

 private:
    BlockInfo *GetBlockInfo(uint8_t *block) { return (BlockInfo *)(block - SHM_BLOCK_INFO_SIZE); }

    BlockInfo *PopSendBlockInfo() {
        BlockInfo *info = NULL;
        return to_send_queue_->try_dequeue(info) ? info : NULL;
    }

    bool RemoveOldShm(const char *shm_name, int32_t timeoutsec) {
        key_t shm_key = ftok(shm_name, 666);
        int32_t shm_id = shmget(shm_key, 0, IPC_CREAT | 0666);
        if (-1 == shm_id) {
            return true;
        }

        DestoryShmReqInfo req_info(shm_id);

        int32_t sec = 0;
        bool notify_ok = false;
        do {
            ipc_client_.request((const uint8_t *)&req_info, sizeof(req_info), true, 1000,
                                [&](uint8_t *data, int len, bool is_last) {
                                    if (len) {
                                        auto base_rsp_info = (BaseRspInfo *)data;
                                        if (CMD_DESTORY == base_rsp_info->cmd) {
                                            notify_ok = ERR_OK == base_rsp_info->err;
                                        }
                                    }
                                });

            ++sec;
        } while (!notify_ok && sec < timeoutsec);

        if (notify_ok) {
            // 通知完成后remove
            int ret = shmctl(shm_id, IPC_RMID, NULL);
            return 0 == ret;
        }

        return false;
    }

    bool InitShareMemory(const char *shm_name, int64_t shm_bytes, int32_t block_bytes) {
        block_cnt_ = shm_bytes / block_bytes;
        int64_t real_shm_bytes = block_cnt_ * (block_bytes + SHM_BLOCK_INFO_SIZE);

        key_t shm_key = ftok(shm_name, 666);
        shm_id_ = shmget(shm_key, real_shm_bytes, IPC_CREAT | 0666);
        shm_addr_ = (uint8_t *)shmat(shm_id_, 0, 0);

        if ((uint8_t *)-1 == shm_addr_) {
            return false;
        }

        free_queue_ = new BlockQueue(block_cnt_);
        to_send_queue_ = new BlockQueue(block_cnt_);

        uint8_t *addr = shm_addr_;
        while (addr < shm_addr_ + real_shm_bytes) {
            BlockInfo *info = (BlockInfo *)addr;
            free_queue_->enqueue(info);

            addr += SHM_BLOCK_INFO_SIZE;
            addr += block_bytes;
        }

        return true;
    }

    static void SendThreadProc(Client *client) { client->OnSendThread(); }

    void OnSendThread() {
        CmdId next_cmd = CmdId::CMD_INIT;

        auto ResponseCb = [&](uint8_t *data, int len, bool is_last) {
            if (len) {
                auto base_rsp_info = (BaseRspInfo *)data;

                switch (base_rsp_info->cmd) {
                    case CMD_INIT:
                        OnInitShmRsp(base_rsp_info, next_cmd);
                        break;
                    case CMD_SUBMIT:
                        OnSubmitBlockRsp(base_rsp_info, next_cmd);
                        break;
                    case CMD_RECYCLE:
                        OnRecycleBlockRsp(base_rsp_info, next_cmd);
                        break;
                    default:
                        break;
                }
            }
        };

        InitShmReqInfo init_info(shm_id_, block_cnt_);
        SubmitBlockReqInfo submit_info(shm_id_);
        RecycleBlockReqInfo recycle_info(shm_id_);

        while (running_) {
            const uint8_t *req_data = NULL;
            int64_t req_len = 0;

            switch (next_cmd) {
                case CMD_INIT:
                    req_data = (const uint8_t *)&init_info;
                    req_len = sizeof(init_info);
                    break;
                case CMD_SUBMIT: {
                    BlockInfo *info = PopSendBlockInfo();
                    if (info) {
                        submit_info.offset = (uint8_t *)info - shm_addr_;

                        req_data = (const uint8_t *)&submit_info;
                        req_len = sizeof(submit_info);

                        send_done_.insert(info);
                    } else if (!send_done_.empty()) {
                        next_cmd = CMD_RECYCLE;

                        // bool loop = (上次回收失败,从回收跳到提交,再到回收,如此循环);
                        // if (loop)
                        { usleep(1); /* 避免ZMQ调度频繁 */ }
                    } else {
                        usleep(1); /* 既没有待发送块，也没有待回收块，避免CPU 100% */
                    }
                } break;
                case CMD_RECYCLE:
                    req_data = (const uint8_t *)&recycle_info;
                    req_len = sizeof(recycle_info);
                    break;
                case CMD_DESTORY:
                    break;
                default:
                    break;
            }

            if (req_data && !ipc_client_.request(req_data, req_len, true, 1000, ResponseCb)) {
                next_cmd = CMD_INIT;
                sleep(1);
            }
        }
    }

    void OnInitShmRsp(BaseRspInfo *info, CmdId &next_cmd) {
        if (ERR_OK == info->err) {
            next_cmd = CMD_SUBMIT;
            for (auto info : send_done_) {
                free_queue_->enqueue(info);
            }
            send_done_.clear();
        } else {
            next_cmd = CMD_INIT;
        }
    }

    void OnSubmitBlockRsp(BaseRspInfo *info, CmdId &next_cmd) {
        if (ERR_FAIL == info->err) {
            // resend
            auto submit_rsp_info = (SubmitBlockRspInfo *)info;
            BlockInfo *info = (BlockInfo *)(shm_addr_ + submit_rsp_info->offset);
            to_send_queue_->enqueue(info);
            send_done_.erase(info);
        } else if (ERR_EXCEPTION == info->err) {
            next_cmd = CMD_INIT;
        }
    }

    void OnRecycleBlockRsp(BaseRspInfo *info, CmdId &next_cmd) {
        if (ERR_OK == info->err) {
            auto recycle_rsp_info = (RecycleBlockRspInfo *)info;
            BlockInfo *info = (BlockInfo *)(shm_addr_ + recycle_rsp_info->offset);
            free_queue_->enqueue(info);
            send_done_.erase(info);
        } else if (ERR_FAIL == info->err) {
            next_cmd = CMD_SUBMIT;
        } else if (ERR_EXCEPTION == info->err) {
            next_cmd = CMD_INIT;
        }
    }

 private:
    const char *last_err_msg_;
    uint8_t *shm_addr_;
    int32_t shm_id_;
    int32_t block_cnt_;
    bool running_;

    ipc::Client ipc_client_;
    thread *thread_;

    BlockQueue *free_queue_;
    BlockQueue *to_send_queue_;
    std::set<BlockInfo *> send_done_;  // 已发送
};                                     // namespace shm

}  // namespace shm
}  // namespace flowsql

#endif
