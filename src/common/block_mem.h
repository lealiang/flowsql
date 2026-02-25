/*
 * Copyright (C) 2020-06 - FAST
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 *
 * Author       : fei.wang@foxmail.com
 * Date         : 2020-11-01 20:16:34
 * LastEditors  : fei.wang
 * LastEditTime : 2020-12-18 14:52:44
 */

#ifndef SRC_0_2_FAST_BLOCK_MEM_H_
#define SRC_0_2_FAST_BLOCK_MEM_H_
#include <stdint.h>

namespace flowsql {
#pragma pack(push)
#pragma pack(1)
struct Mem {
    int32_t proc_ident_;    // process identity
    int32_t thread_ident_;  // thread identity
    int32_t serial_no_;     // serial_no
    int32_t free_sec_;      // free seconds?
    int32_t called_cnt_;    // called count
    int32_t data_len_;      // total data length
    int32_t data_off_;      // data offset for write
    int32_t read_off_;      // read offset (for read)
};
#pragma pack(pop)
}  // namespace flowsql

#endif  // SRC_0_2_FAST_BLOCK_MEM_H_
