/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2020-08-17 03:26:29
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */

#ifndef _BASE_SHM_TYPEDEF_H_
#define _BASE_SHM_TYPEDEF_H_

#include <stdint.h>
#include "concurrentqueue/concurrentqueue.h"

namespace flowsql {
namespace shm {

#define SHM_BLOCK_INFO_SIZE sizeof(BlockInfo)
struct BlockInfo {
    int32_t data_len;

    uint8_t *GetBlock() { return (uint8_t *)this + SHM_BLOCK_INFO_SIZE; }
};

typedef moodycamel::ConcurrentQueue<BlockInfo *> BlockQueue;

enum CmdId {
    CMD_INIT = 0,  // 建连初始化
    CMD_SUBMIT,    // 提交block
    CMD_RECYCLE,   // 回收block
    CMD_DESTORY,   // client主动销毁共享内存
    CMD_MAX
};

enum ErrorNo {
    ERR_OK = 0,    // 正常
    ERR_FAIL,      // 请求未处理
    ERR_EXCEPTION  // 异常，需要重连
};

struct BaseReqInfo {
    BaseReqInfo(int32_t _shm_id, CmdId _cmd) : shm_id(_shm_id), cmd(_cmd) {}

    int32_t shm_id;  // 共享内存ID
    CmdId cmd;       // enum CmdId
};

struct BaseRspInfo {
    BaseRspInfo(int32_t _shm_id, CmdId _cmd, ErrorNo _err) : shm_id(_shm_id), cmd(_cmd), err(_err) {}

    int32_t shm_id;  // 共享内存ID
    int32_t cmd;     // enum CmdId
    ErrorNo err;     // enum ErrorNo
};

struct InitShmReqInfo : public BaseReqInfo {
    InitShmReqInfo(int32_t _shm_id, int32_t _block_cnt) : BaseReqInfo(_shm_id, CMD_INIT), block_cnt(_block_cnt) {}

    int32_t block_cnt;
};

struct InitShmRspInfo : public BaseRspInfo {
    InitShmRspInfo(int32_t _shm_id, ErrorNo _err) : BaseRspInfo(_shm_id, CMD_INIT, _err) {}
};

struct SubmitBlockReqInfo : public BaseReqInfo {
    SubmitBlockReqInfo(int32_t _shm_id) : BaseReqInfo(_shm_id, CMD_SUBMIT) {}

    int64_t offset;  // block在共享内存中的偏移位置
};

struct SubmitBlockRspInfo : public BaseRspInfo {
    SubmitBlockRspInfo(int32_t _shm_id, ErrorNo _err) : BaseRspInfo(_shm_id, CMD_SUBMIT, _err) {}

    uint64_t offset;  // block在共享内存中的偏移位置
};

struct RecycleBlockReqInfo : public BaseReqInfo {
    RecycleBlockReqInfo(int32_t _shm_id) : BaseReqInfo(_shm_id, CMD_RECYCLE) {}
};

struct RecycleBlockRspInfo : public BaseRspInfo {
    RecycleBlockRspInfo(int32_t _shm_id, ErrorNo _err) : BaseRspInfo(_shm_id, CMD_RECYCLE, _err) {}

    uint64_t offset;  // block在共享内存中的偏移位置
};

struct DestoryShmReqInfo : public BaseReqInfo {
    DestoryShmReqInfo(int32_t _shm_id) : BaseReqInfo(_shm_id, CMD_DESTORY) {}
};

struct DestoryShmRspInfo : public BaseRspInfo {
    DestoryShmRspInfo(int32_t _shm_id, ErrorNo _err) : BaseRspInfo(_shm_id, CMD_DESTORY, _err) {}
};

}  // namespace shm
}  // namespace flowsql

#endif
