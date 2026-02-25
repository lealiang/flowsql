/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2020-11-01 20:16:34
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */

#ifndef _FLOWSQL_COMMON_BLOCK_MEM_H_
#define _FLOWSQL_COMMON_BLOCK_MEM_H_
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

#endif  // _FLOWSQL_COMMON_BLOCK_MEM_H_
